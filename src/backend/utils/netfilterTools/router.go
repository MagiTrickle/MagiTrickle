package netfilterTools

import (
	"errors"
	"fmt"
	"slices"
	"sync"

	"magitrickle/utils/iptables"

	"github.com/rs/zerolog/log"
	"github.com/vishvananda/netlink"
)

var ErrRouterDisabled = errors.New("router is disabled")

// RouteError — маршрут Name поднять не удалось. Остальные маршруты при
// этом подготовлены.
type RouteError struct {
	Name string
	Err  error
}

func (e *RouteError) Error() string {
	return fmt.Sprintf("failed to enable route %s: %v", e.Name, e.Err)
}

func (e *RouteError) Unwrap() error {
	return e.Err
}

// Встроенные цепочки, из которых Router вызывает цепочки маршрутов.
var routerHooks = []struct{ table, chain string }{
	{"filter", "FORWARD"},
	{"mangle", "PREROUTING"},
	{"nat", "POSTROUTING"},
}

// RouteSpec описывает маршрут: трафик на адреса из ipset Name уходит в
// интерфейс Interface. Name должен быть уникален в пределах Router.
type RouteSpec struct {
	Name      string
	Interface string
}

// routeSet — ipset маршрута. Наполняется только добавлением: маршрут с
// другим содержимым получает новый ipset (см. Router).
type routeSet interface {
	AddIPv4Subnet(subnet IPv4Subnet, ttl IPSetTimeout) error
	AddIPv6Subnet(subnet IPv6Subnet, ttl IPSetTimeout) error

	enable() error
	disable() error
	// swap атомарно обменивает содержимое двух наборов (см. IPSet.Swap).
	swap(other routeSet) error
}

// routeLink — привязка ipset маршрута к интерфейсу: цепочки iptables,
// правило и таблица маршрутизации.
type routeLink interface {
	enable() error
	disable() error
	chainName() string
	linkUpHook(event netlink.LinkUpdate) error
	addrChangeHook(event netlink.AddrUpdate) error
}

// Route — поднятый маршрут. Через него наполняется ipset; после снятия
// маршрута все операции становятся пустыми.
type Route struct {
	routeSet
	link routeLink
	spec RouteSpec
	slot int
	// replaces — действующий маршрут, содержимое которого этот заменит при
	// Commit. Привязка к интерфейсу у них общая, свой у этого только ipset.
	replaces *Route
}

func (r *Route) Spec() RouteSpec {
	return r.spec
}

// netlinkSet — ipset в настоящем ядре.
type netlinkSet struct{ *IPSet }

// enable создаёт ipset. ipset с тем же именем, оставшийся от прошлых
// запусков, пересоздаётся.
func (s netlinkSet) enable() error  { return s.IPSet.Enable() }
func (s netlinkSet) disable() error { return s.IPSet.Disable() }
func (s netlinkSet) swap(other routeSet) error {
	return s.IPSet.Swap(other.(netlinkSet).IPSet)
}

// netlinkLink — привязка к интерфейсу в настоящем ядре. Остатки прошлых
// запусков здесь не чистятся: цепочки убирает CleanIPTables при старте.
type netlinkLink struct{ *IPSetToLink }

func (l netlinkLink) enable() error     { return l.IPSetToLink.Enable() }
func (l netlinkLink) disable() error    { return l.IPSetToLink.Disable() }
func (l netlinkLink) chainName() string { return l.IPSetToLink.ChainName() }
func (l netlinkLink) linkUpHook(event netlink.LinkUpdate) error {
	return l.IPSetToLink.LinkUpHook(event)
}
func (l netlinkLink) addrChangeHook(event netlink.AddrUpdate) error {
	return l.IPSetToLink.AddrChangeHook(event)
}

// Router держит упорядоченный список маршрутов.
//
// Встроенные цепочки (FORWARD, mangle PREROUTING, nat POSTROUTING) вызывают
// одну диспетчерскую цепочку Router'а, а та — цепочки маршрутов строго в
// заданном порядке. Пакет проходит все маршруты подряд, поэтому при
// пересечении метку оставляет последний совпавший: чем позже маршрут в
// списке, тем выше его приоритет.
//
// Маршруты меняются в две фазы. Prepare поднимает то, что нужно, не трогая
// трафик; вызывающий успевает наполнить новые ipset'ы. Commit разом
// переключает трафик и только потом снимает лишнее. Экземпляры маршрута
// живут в двух слотах (<имя>-0 и <имя>-1):
//   - при смене интерфейса новый экземпляр целиком поднимается во втором
//     слоте, и Commit переключает на него диспетчер;
//   - при замене содержимого во втором слоте поднимается только ipset, и
//     Commit атомарно подменяет им действующий (ipset swap), не трогая
//     iptables.
//
// Так трафик не видит ни частично наполненных ipset'ов, ни промежутка без
// маршрута, а неудавшийся Commit оставляет всё как было.
type Router struct {
	mu        sync.Mutex
	nh        *Helper
	chainName string
	enabled   bool
	newSet    func(name string) routeSet
	newLink   func(name, iface string) routeLink

	active   []*Route          // в диспетчере, в его порядке
	prepared map[string]*Route // подняты Prepare и ждут Commit

	// dispatched — цепочки, которые сейчас вызывает диспетчер, если
	// dispatchedValid; иначе состояние диспетчера неизвестно.
	dispatched      []string
	dispatchedValid bool
}

func (nh *Helper) Router(name string) *Router {
	return &Router{
		nh:        nh,
		chainName: nh.ChainPrefix + name,
		newSet: func(name string) routeSet {
			return netlinkSet{nh.IPSet(name)}
		},
		newLink: func(name, iface string) routeLink {
			return netlinkLink{nh.IPSetToLink(name, iface, nh.IpsetPrefix+name)}
		},
		prepared: make(map[string]*Route),
	}
}

// Enable создаёт диспетчерские цепочки и подключает их к встроенным.
// Встроенные цепочки должны быть заранее зарегистрированы через
// RegisterChainPatch.
func (rt *Router) Enable() error {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	if rt.enabled {
		return nil
	}
	for _, ipt := range rt.nh.iptables() {
		if err := rt.hook(ipt); err != nil {
			for _, ipt := range rt.nh.iptables() {
				_ = rt.unhook(ipt)
			}
			return err
		}
	}
	rt.enabled = true
	rt.dispatched, rt.dispatchedValid = nil, true
	return nil
}

// Disable снимает все маршруты и удаляет диспетчерские цепочки.
func (rt *Router) Disable() error {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	if !rt.enabled {
		return nil
	}
	rt.enabled = false

	// Сначала убираем диспетчер (unhook очищает и удаляет его цепочку):
	// пока он ссылается на цепочки маршрутов, их и ipset'ы не удалить.
	var errs []error
	for _, ipt := range rt.nh.iptables() {
		errs = append(errs, rt.unhook(ipt))
	}
	rt.dispatched, rt.dispatchedValid = nil, false
	rt.release(nil)
	rt.active, rt.prepared = nil, make(map[string]*Route)
	return errors.Join(errs...)
}

// Prepare готовит маршруты для specs, не меняя трафик:
//   - маршрут, который уже в диспетчере с тем же интерфейсом, возвращается
//     как есть, если его имени нет в refill;
//   - для имени из refill поднимается пустой ipset, которым Commit заменит
//     содержимое действующего маршрута;
//   - для нового маршрута или маршрута со сменившимся интерфейсом
//     поднимается новый экземпляр с пустым ipset.
//
// Возвращает маршруты в порядке specs. Если маршрут поднять не удалось, на
// его месте nil, а в ошибке есть *RouteError с его именем. Если не
// возвращено ничего, ничего и не изменилось. Всё, что подготовил прошлый
// Prepare и не взял Commit, снимается.
func (rt *Router) Prepare(specs []RouteSpec, refill map[string]bool) ([]*Route, error) {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	if !rt.enabled {
		return nil, ErrRouterDisabled
	}
	names := make(map[string]struct{}, len(specs))
	for _, spec := range specs {
		if _, ok := names[spec.Name]; ok {
			return nil, fmt.Errorf("duplicate route name %q", spec.Name)
		}
		names[spec.Name] = struct{}{}
	}

	rt.release(rt.active)
	rt.prepared = make(map[string]*Route)

	active := rt.activeByName()
	result := make([]*Route, len(specs))
	var errs []error
	for i, spec := range specs {
		current := active[spec.Name]
		var route *Route
		var err error
		switch {
		case current != nil && current.spec == spec && !refill[spec.Name]:
			result[i] = current
			continue
		case current != nil && current.spec == spec:
			route, err = rt.stage(current)
		default:
			route, err = rt.bringUp(spec, current)
		}
		if err != nil {
			errs = append(errs, &RouteError{Name: spec.Name, Err: err})
			continue
		}
		rt.prepared[spec.Name] = route
		result[i] = route
	}
	return result, errors.Join(errs...)
}

// bringUp поднимает новый экземпляр маршрута в слоте, свободном от current.
func (rt *Router) bringUp(spec RouteSpec, current *Route) (*Route, error) {
	slot := 0
	if current != nil {
		slot = 1 - current.slot
	}
	name := instanceName(spec.Name, slot)
	set := rt.newSet(name)
	if err := set.enable(); err != nil {
		return nil, fmt.Errorf("failed to initialize ipset: %w", err)
	}
	link := rt.newLink(name, spec.Interface)
	if err := link.enable(); err != nil {
		_ = link.disable()
		_ = set.disable()
		return nil, fmt.Errorf("failed to link ipset to interface: %w", err)
	}
	return &Route{routeSet: set, link: link, spec: spec, slot: slot}, nil
}

// stage поднимает пустой ipset во втором слоте current. После Commit он
// займёт место ipset'а current, поэтому маршрут сразу числится в слоте current.
func (rt *Router) stage(current *Route) (*Route, error) {
	set := rt.newSet(instanceName(current.spec.Name, 1-current.slot))
	if err := set.enable(); err != nil {
		return nil, fmt.Errorf("failed to initialize ipset: %w", err)
	}
	return &Route{routeSet: set, link: current.link, spec: current.spec, slot: current.slot, replaces: current}, nil
}

func instanceName(name string, slot int) string {
	return fmt.Sprintf("%s-%d", name, slot)
}

// Commit делает действующими routes в этом порядке и снимает всё
// остальное. Каждый маршрут должен быть действующим или взятым из
// последнего Prepare; для имени, у которого новый маршрут не поднялся или
// не наполнился, вызывающий передаёт прежний, и трафик идёт по нему.
//
// Сначала подменяется содержимое ipset'ов, потом переключается диспетчер.
// Если что-то не удалось, возвращается прежнее состояние. Ошибки снятия
// лишнего только логируются: на трафик оно уже не влияет.
func (rt *Router) Commit(routes []*Route) error {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	if !rt.enabled {
		return ErrRouterDisabled
	}
	known := make(map[*Route]struct{})
	for _, route := range rt.all() {
		known[route] = struct{}{}
	}
	names := make(map[string]struct{}, len(routes))
	for _, route := range routes {
		if _, ok := known[route]; !ok {
			return fmt.Errorf("route %s is neither active nor prepared", route.spec.Name)
		}
		if _, ok := names[route.spec.Name]; ok {
			return fmt.Errorf("duplicate route name %q", route.spec.Name)
		}
		names[route.spec.Name] = struct{}{}
	}

	var swapped []*Route
	for _, route := range routes {
		if route.replaces == nil {
			continue
		}
		if err := route.swap(route.replaces.routeSet); err != nil {
			return errors.Join(fmt.Errorf("route %s: %w", route.spec.Name, err), rt.unswap(swapped))
		}
		swapped = append(swapped, route)
	}

	if err := rt.dispatch(routes); err != nil {
		// Возвращаем прежнее состояние, чтобы подготовленное можно было
		// снять следующим Prepare.
		if restoreErr := rt.dispatch(rt.active); restoreErr != nil {
			err = errors.Join(err, fmt.Errorf("failed to restore dispatcher: %w", restoreErr))
		}
		return errors.Join(err, rt.unswap(swapped))
	}

	for _, route := range swapped {
		route.replaces = nil
	}
	rt.release(routes)
	rt.active, rt.prepared = slices.Clone(routes), make(map[string]*Route)
	return nil
}

// unswap возвращает на место содержимое, подменённое Commit.
func (rt *Router) unswap(swapped []*Route) error {
	var errs []error
	for _, route := range swapped {
		if err := route.swap(route.replaces.routeSet); err != nil {
			errs = append(errs, fmt.Errorf("route %s: failed to swap ipset back: %w", route.spec.Name, err))
		}
	}
	return errors.Join(errs...)
}

// LinkUpHook передаёт событие поднятия интерфейса маршрутам через него.
func (rt *Router) LinkUpHook(event netlink.LinkUpdate) error {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	return rt.eachLink(event.Link.Attrs().Name, func(link routeLink) error {
		return link.linkUpHook(event)
	})
}

// AddrChangeHook передаёт событие смены адреса интерфейса ifaceName
// маршрутам через него.
func (rt *Router) AddrChangeHook(ifaceName string, event netlink.AddrUpdate) error {
	rt.mu.Lock()
	defer rt.mu.Unlock()

	return rt.eachLink(ifaceName, func(link routeLink) error {
		return link.addrChangeHook(event)
	})
}

// eachLink вызывает fn для каждой привязки к интерфейсу ifaceName по разу.
func (rt *Router) eachLink(ifaceName string, fn func(routeLink) error) error {
	seen := make(map[routeLink]struct{})
	var errs []error
	for _, route := range rt.all() {
		if _, ok := seen[route.link]; ok || route.spec.Interface != ifaceName {
			continue
		}
		seen[route.link] = struct{}{}
		if err := fn(route.link); err != nil {
			errs = append(errs, fmt.Errorf("route %s: %w", route.spec.Name, err))
		}
	}
	return errors.Join(errs...)
}

// all возвращает все поднятые маршруты: и действующие, и подготовленные.
func (rt *Router) all() []*Route {
	list := slices.Clone(rt.active)
	for _, route := range rt.prepared {
		list = append(list, route)
	}
	return list
}

func (rt *Router) activeByName() map[string]*Route {
	byName := make(map[string]*Route, len(rt.active))
	for _, route := range rt.active {
		byName[route.spec.Name] = route
	}
	return byName
}

// release снимает все маршруты, кроме keep. Привязку к интерфейсу, общую с
// маршрутом из keep, оставляет.
func (rt *Router) release(keep []*Route) {
	kept := make(map[*Route]struct{}, len(keep))
	done := make(map[routeLink]struct{}, len(keep))
	for _, route := range keep {
		kept[route] = struct{}{}
		done[route.link] = struct{}{}
	}
	for _, route := range rt.all() {
		if _, ok := kept[route]; ok {
			continue
		}
		if _, ok := done[route.link]; !ok {
			done[route.link] = struct{}{}
			if err := route.link.disable(); err != nil {
				log.Error().Err(err).Str("route", route.spec.Name).Int("slot", route.slot).Msg("failed to unlink route from interface")
			}
		}
		if err := route.disable(); err != nil {
			log.Error().Err(err).Str("route", route.spec.Name).Int("slot", route.slot).Msg("failed to destroy route ipset")
		}
	}
}

// dispatch переписывает диспетчерские цепочки так, чтобы они вызывали
// цепочки routes в заданном порядке. Если порядок уже такой, ничего не делает.
func (rt *Router) dispatch(routes []*Route) error {
	chains := make([]string, len(routes))
	for i, route := range routes {
		chains[i] = route.link.chainName()
	}
	if rt.dispatchedValid && slices.Equal(chains, rt.dispatched) {
		return nil
	}

	rt.dispatchedValid = false
	var errs []error
	for _, ipt := range rt.nh.iptables() {
		errs = append(errs, rt.dispatchTo(ipt, chains))
	}
	if err := errors.Join(errs...); err != nil {
		return err
	}
	rt.dispatched, rt.dispatchedValid = chains, true
	return nil
}

func (rt *Router) dispatchTo(ipt *iptables.IPTables, chains []string) error {
	jumps := make([][]string, len(chains))
	for i, chain := range chains {
		jumps[i] = []string{"-j", chain}
	}
	// Диспетчер — одна цепочка на весь трафик, поэтому содержимое меняется
	// атомарно: параллельный Commit (например, из хука netfilter.d) не должен
	// увидеть её пустой.
	for _, hook := range routerHooks {
		if err := ipt.SetChain(hook.table, rt.chainName, jumps); err != nil {
			return fmt.Errorf("failed to set chain: %w", err)
		}
	}
	if err := ipt.Commit(); err != nil {
		return fmt.Errorf("failed to commit iptables rules: %w", err)
	}
	return nil
}

func (rt *Router) hook(ipt *iptables.IPTables) error {
	for _, hook := range routerHooks {
		if err := ipt.RegisterChainOverride(hook.table, rt.chainName); err != nil {
			return fmt.Errorf("failed to create chain: %w", err)
		}
		if err := ipt.Append(hook.table, hook.chain, "-j", rt.chainName); err != nil {
			return fmt.Errorf("failed to link chain to %s %s: %w", hook.table, hook.chain, err)
		}
	}
	if err := ipt.Commit(); err != nil {
		return fmt.Errorf("failed to commit iptables rules: %w", err)
	}
	return nil
}

func (rt *Router) unhook(ipt *iptables.IPTables) error {
	var errs []error
	for _, hook := range routerHooks {
		if err := ipt.Delete(hook.table, hook.chain, "-j", rt.chainName); err != nil {
			errs = append(errs, fmt.Errorf("failed to unlink chain from %s %s: %w", hook.table, hook.chain, err))
		}
		if err := ipt.RegisterChainDelete(hook.table, rt.chainName); err != nil {
			errs = append(errs, fmt.Errorf("failed to delete chain: %w", err))
		}
	}
	if err := ipt.Commit(); err != nil {
		errs = append(errs, fmt.Errorf("failed to commit iptables rules: %w", err))
	}
	return errors.Join(errs...)
}
