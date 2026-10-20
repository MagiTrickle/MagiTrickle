// Package routing связывает наборы правил с маршрутами netfilter.
//
// Наборы правил (rulesets) не знают, как устроена маршрутизация, а
// netfilterTools не знает, откуда берутся маршруты. Table сопоставляет
// каждому набору маршрут, наполняет его адресами из DNS и держит порядок
// маршрутов равным порядку наборов.
package routing

import (
	"errors"
	"fmt"
	"maps"
	"net"
	"net/netip"
	"slices"
	"sync"

	"magitrickle/rulesets"
	"magitrickle/utils/netfilterTools"
	"magitrickle/utils/recordsCache"

	"github.com/rs/zerolog/log"
)

// Route — маршрут, в ipset которого складываются адреса набора правил.
type Route interface {
	AddIPv4Subnet(subnet netfilterTools.IPv4Subnet, ttl netfilterTools.IPSetTimeout) error
	AddIPv6Subnet(subnet netfilterTools.IPv6Subnet, ttl netfilterTools.IPSetTimeout) error
}

// Router меняет маршруты в две фазы (см. netfilterTools.Router).
//
// Prepare, не трогая трафик, возвращает маршруты в порядке specs: пока
// интерфейс не меняется и имени нет в refill — действующий, иначе — новый
// с пустым ipset. Не поднявшийся маршрут возвращается как nil вместе с
// *netfilterTools.RouteError; nil вместо всего списка означает, что ничего
// не изменилось.
//
// Commit разом делает действующими переданные маршруты (из Prepare или
// прежние) в этом порядке и снимает всё остальное; при ошибке всё остаётся
// как было.
type Router interface {
	Prepare(specs []netfilterTools.RouteSpec, refill map[string]bool) ([]Route, error)
	Commit(routes []Route) error
}

// Records — кэш DNS-записей, из которого наполняются новые маршруты.
type Records interface {
	ListKnownDomains() []string
	GetAddresses(domainName string) []*recordsCache.Address
}

// Address — адрес из DNS-ответа и время его жизни в секундах.
type Address struct {
	IP  net.IP
	TTL uint32
}

// Table — упорядоченный список наборов правил с их маршрутами.
type Table struct {
	router  Router
	records Records

	// applyMu упорядочивает вызовы Apply и защищает их состояние.
	applyMu sync.Mutex
	// requested — наборы из последнего Apply по ключу, включая не применившиеся.
	requested map[string]rulesets.RuleSet
	// unsynced — наборы, действующий маршрут которых наполнен не полностью.
	unsynced map[string]bool
	// pending — последний Apply применил не всё, и его стоит повторить.
	pending bool

	// mu защищает только entries: DNS-ответы ждут его лишь на время подмены
	// списка, а не на время наполнения маршрутов.
	mu      sync.RWMutex
	entries []entry

	// journals — адреса, которые DNS добавил в маршрут, пока тот наполняется
	// (см. fill).
	journalMu sync.Mutex
	journals  map[Route][]Address
}

// ApplyError — Apply выполнен не полностью.
type ApplyError struct {
	// Fatal — маршрутизация в целом не применена: трафик идёт по прежним
	// маршрутам, и ни одно изменение не вступило в силу.
	Fatal error
	// RuleSets — ошибки изменившихся наборов по ключу. Остальные наборы
	// применены.
	RuleSets map[string]error
}

func (e *ApplyError) Error() string {
	return errors.Join(e.Unwrap()...).Error()
}

func (e *ApplyError) Unwrap() []error {
	var list []error
	if e.Fatal != nil {
		list = append(list, e.Fatal)
	}
	for _, key := range slices.Sorted(maps.Keys(e.RuleSets)) {
		list = append(list, e.RuleSets[key])
	}
	return list
}

type entry struct {
	ruleSet rulesets.RuleSet
	route   Route
	// previous — прежние правила и маршрут набора, которые несут трафик, пока
	// новый route наполняется и ждёт Commit. DNS-ответы проверяются по обоим
	// спискам правил и пишутся каждый в свой маршрут: клиент может
	// обратиться к адресу ещё через старый маршрут, а если Commit не удастся,
	// старый так и останется — и в нём не должно быть лишнего.
	previous *entry
}

func New(router Router, records Records) *Table {
	return &Table{router: router, records: records}
}

// Apply делает активными ruleSets в заданном порядке. Маршрут набора,
// который не изменился, остаётся как есть. Для нового набора или набора с
// изменившимися правилами или интерфейсом роутер выдаёт маршрут с пустым
// ipset; он наполняется из кэша DNS, и трафик переключается на него только
// после этого, атомарно. Действующие маршруты при этом не меняются.
//
// Ошибка (*ApplyError) возвращается, если роутер не смог применить порядок
// или если не применился набор, изменившийся с прошлого вызова, — такое
// изменение вызывающему стоит откатить; маршрут набора при этом остаётся
// прежним. Если снова не применился набор, который не менялся (например,
// маршрут через сломанный интерфейс), ошибка только логируется: набор
// повторяется при каждом Apply, но не блокирует изменения других наборов.
func (t *Table) Apply(ruleSets []rulesets.RuleSet) error {
	t.applyMu.Lock()
	defer t.applyMu.Unlock()

	// entries меняет только Apply, поэтому читать их под applyMu можно без mu.
	previous := make(map[string]entry, len(t.entries))
	for _, e := range t.entries {
		previous[e.ruleSet.Key()] = e
	}

	specs := make([]netfilterTools.RouteSpec, len(ruleSets))
	refill := make(map[string]bool)
	for i, rs := range ruleSets {
		specs[i] = netfilterTools.RouteSpec{Name: rs.Key(), Interface: rs.Interface}
		if prev, ok := previous[rs.Key()]; ok && (t.unsynced[rs.Key()] || !prev.ruleSet.SameRules(rs)) {
			refill[rs.Key()] = true
		}
	}
	routes, err := t.router.Prepare(specs, refill)
	if routes == nil {
		// Роутер ничего не изменил — прежние маршруты на месте, как и entries.
		return &ApplyError{Fatal: errors.Join(err, errors.New("router prepared nothing"))}
	}
	if len(routes) != len(ruleSets) {
		return &ApplyError{Fatal: errors.Join(err, fmt.Errorf("router returned %d routes for %d rule sets", len(routes), len(ruleSets)))}
	}

	routeErrs, fatal := splitRouteErrors(err)
	failed := make(map[string]error)
	pending := false
	report := func(rs rulesets.RuleSet, err error) {
		pending = true
		if t.changed(rs) {
			failed[rs.Key()] = err
			return
		}
		log.Error().Err(err).Str("ruleSetId", rs.Key()).Str("ruleSetName", rs.Name).Msg("rule set is still not applied")
	}

	unsynced := make(map[string]bool)
	entries := make([]entry, 0, len(ruleSets))
	var toFill []entry
	for i, rs := range ruleSets {
		prev, hadPrev := previous[rs.Key()]
		if routes[i] == nil {
			err := routeErrs[rs.Key()]
			if err == nil {
				err = fmt.Errorf("route %s was not prepared", rs.Key())
			}
			report(rs, err)
			if hadPrev {
				// Остаётся прежний маршрут, и DNS-ответы должны идти в него.
				entries = append(entries, prev)
				unsynced[rs.Key()] = t.unsynced[rs.Key()]
			}
			continue
		}

		e := entry{ruleSet: rs, route: routes[i]}
		if hadPrev && prev.route == e.route {
			// Правила те же — оставляем прежний Matcher с уже скомпилированными выражениями.
			e.ruleSet.Domains = prev.ruleSet.Domains
		} else {
			if hadPrev {
				e.previous = &prev
			}
			toFill = append(toFill, e)
		}
		entries = append(entries, e)
	}

	// Публикуем до наполнения: DNS-ответы, пришедшие во время fill, попадут
	// и в новые маршруты. На трафик это не влияет до Commit.
	previousEntries := t.entries
	t.mu.Lock()
	t.entries = entries
	t.mu.Unlock()

	fillErrs := t.fill(toFill)
	committed := make([]entry, 0, len(entries))
	commit := make([]Route, 0, len(entries))
	for _, e := range entries {
		if err := fillErrs[e.ruleSet.Key()]; err != nil {
			report(e.ruleSet, fmt.Errorf("failed to fill rule set %s: %w", e.ruleSet.Key(), err))
			if e.previous != nil {
				// Недозаполненный маршрут не подменяет прежний.
				e = *e.previous
				unsynced[e.ruleSet.Key()] = t.unsynced[e.ruleSet.Key()]
			} else {
				// Прежнего нет: неполный маршрут лучше никакого, досинхронизируем при следующем Apply.
				unsynced[e.ruleSet.Key()] = true
			}
		}
		e.previous = nil
		committed = append(committed, e)
		commit = append(commit, e.route)
	}

	if err := t.router.Commit(commit); err != nil {
		// Роутер оставил всё как было, значит и DNS-ответы должны идти в
		// прежние маршруты. requested и unsynced не обновляем: следующий
		// Apply повторит всё заново.
		t.mu.Lock()
		t.entries = previousEntries
		t.mu.Unlock()
		t.pending = true
		return &ApplyError{Fatal: errors.Join(fatal, err), RuleSets: failed}
	}

	t.mu.Lock()
	t.entries = committed
	t.mu.Unlock()

	t.pending = pending
	t.unsynced = unsynced
	t.requested = make(map[string]rulesets.RuleSet, len(ruleSets))
	for _, rs := range ruleSets {
		t.requested[rs.Key()] = rs
	}
	if fatal != nil || len(failed) > 0 {
		return &ApplyError{Fatal: fatal, RuleSets: failed}
	}
	return nil
}

// Pending сообщает, что прошлый Apply применил не всё: какой-то набор не
// поднялся или не наполнился. Повторный Apply с теми же наборами повторит их.
func (t *Table) Pending() bool {
	t.applyMu.Lock()
	defer t.applyMu.Unlock()
	return t.pending
}

// changed сообщает, отличается ли набор от запрошенного в прошлом Apply.
func (t *Table) changed(rs rulesets.RuleSet) bool {
	prev, ok := t.requested[rs.Key()]
	return !ok || prev.Interface != rs.Interface || !prev.SameRules(rs)
}

// splitRouteErrors разбирает ошибку роутера на ошибки отдельных маршрутов
// и всё остальное.
func splitRouteErrors(err error) (byName map[string]error, rest error) {
	var list []error
	if joined, ok := err.(interface{ Unwrap() []error }); ok {
		list = joined.Unwrap()
	} else if err != nil {
		list = []error{err}
	}

	byName = make(map[string]error)
	var others []error
	for _, err := range list {
		var routeErr *netfilterTools.RouteError
		if errors.As(err, &routeErr) {
			byName[routeErr.Name] = err
		} else {
			others = append(others, err)
		}
	}
	return byName, errors.Join(others...)
}

// AddAddresses направляет адреса во все наборы правил, которые совпадают
// хотя бы с одним из имён. names[0] — имя из DNS-ответа, остальные — его
// псевдонимы.
func (t *Table) AddAddresses(names []string, addresses []Address) {
	if len(names) == 0 {
		return
	}

	t.mu.RLock()
	defer t.mu.RUnlock()

	for _, e := range t.entries {
		if e.previous != nil && e.previous.ruleSet.Matches(names) {
			for _, address := range addresses {
				if isPermanentHost(e.previous.ruleSet, address.IP) {
					continue
				}
				if err := addAddress(e.previous.route, address); err != nil {
					log.Error().Err(err).Str("address", address.IP.String()).Str("ruleSetId", e.ruleSet.Key()).Msg("failed to add address to the outgoing route")
				}
			}
		}

		if !e.ruleSet.Matches(names) {
			continue
		}
		log.Debug().
			Strs("names", names).
			Str("ruleSetId", e.ruleSet.Key()).
			Str("ruleSetName", e.ruleSet.Name).
			Int("addresses", len(addresses)).
			Msg("matched rule set")

		for _, address := range addresses {
			if isPermanentHost(e.ruleSet, address.IP) {
				// Хост уже в маршруте бессрочно по subnet-правилу; временная
				// запись из DNS заменила бы его и со временем истекла.
				continue
			}
			t.journal(e.route, address)
			if err := addAddress(e.route, address); err != nil {
				log.Error().
					Err(err).
					Str("name", names[0]).
					Str("address", address.IP.String()).
					Str("ruleSetId", e.ruleSet.Key()).
					Str("ruleSetName", e.ruleSet.Name).
					Msg("failed to add address to routing")
				continue
			}
			log.Info().
				Str("name", names[0]).
				Str("address", address.IP.String()).
				Str("ruleSetId", e.ruleSet.Key()).
				Str("ruleSetName", e.ruleSet.Name).
				Msg("added to routing")
		}
	}
}

func isPermanentHost(rs rulesets.RuleSet, ip net.IP) bool {
	addr, ok := netip.AddrFromSlice(ip)
	return ok && rs.PermanentHost(addr)
}

func addAddress(route Route, address Address) error {
	ttl := address.TTL
	switch len(address.IP) {
	case net.IPv4len:
		return route.AddIPv4Subnet(netfilterTools.IPv4Subnet{Address: [4]byte(address.IP)}, &ttl)
	case net.IPv6len:
		return route.AddIPv6Subnet(netfilterTools.IPv6Subnet{Address: [16]byte(address.IP)}, &ttl)
	}
	return fmt.Errorf("unexpected address length %d", len(address.IP))
}

type netfilterRouter struct {
	router *netfilterTools.Router
}

// FromNetfilter представляет netfilterTools.Router как Router.
func FromNetfilter(router *netfilterTools.Router) Router {
	return netfilterRouter{router: router}
}

func (r netfilterRouter) Prepare(specs []netfilterTools.RouteSpec, refill map[string]bool) ([]Route, error) {
	routes, err := r.router.Prepare(specs, refill)
	if routes == nil {
		return nil, err
	}
	out := make([]Route, len(routes))
	for i, route := range routes {
		if route != nil {
			out[i] = route
		}
	}
	return out, err
}

func (r netfilterRouter) Commit(routes []Route) error {
	list := make([]*netfilterTools.Route, len(routes))
	for i, route := range routes {
		nfRoute, ok := route.(*netfilterTools.Route)
		if !ok {
			return fmt.Errorf("route %T was not prepared by this router", route)
		}
		list[i] = nfRoute
	}
	return r.router.Commit(list)
}
