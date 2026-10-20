//go:build testing

package netfilterTools

import (
	"errors"
	"reflect"
	"testing"

	"magitrickle/utils/iptables"

	"github.com/vishvananda/netlink"
)

func newTestRouter(t *testing.T) (*Router, *iptables.FakeIPTables) {
	t.Helper()

	fake := iptables.NewFakeIPTables(iptables.ProtocolIPv4)
	for _, hook := range routerHooks {
		fake.SetInitialRules(hook.table, hook.chain, [][]string{{"-j", "FOREIGN"}})
	}
	ipt := iptables.NewIPTables(fake)
	for _, hook := range routerHooks {
		if err := ipt.RegisterChainPatch(hook.table, hook.chain); err != nil {
			t.Fatal(err)
		}
	}

	nh := &Helper{ChainPrefix: "MT_", IPTables4: ipt}
	router := nh.Router("ROUTING")
	if err := router.Enable(); err != nil {
		t.Fatalf("Enable: %v", err)
	}
	return router, fake
}

func testRoute(nh *Helper, name string) *Route {
	return &Route{routeSet: newFakeSet(name), link: &fakeLink{chain: nh.ChainPrefix + name}, spec: RouteSpec{Name: name}}
}

func TestRouterHooksDispatchChain(t *testing.T) {
	_, fake := newTestRouter(t)

	for _, hook := range routerHooks {
		want := [][]string{{"-j", "FOREIGN"}, {"-j", "MT_ROUTING"}}
		if got := fake.GetRules(hook.table, hook.chain); !reflect.DeepEqual(got, want) {
			t.Errorf("%s %s = %v, want %v", hook.table, hook.chain, got, want)
		}
		if !fake.ChainExists(hook.table, "MT_ROUTING") {
			t.Errorf("%s MT_ROUTING does not exist", hook.table)
		}
	}
}

func TestRouterDispatchKeepsGivenOrder(t *testing.T) {
	router, fake := newTestRouter(t)
	sub := testRoute(router.nh, "s1")
	first := testRoute(router.nh, "g1")
	second := testRoute(router.nh, "g2")

	for _, order := range [][]*Route{
		{sub, first, second},
		{sub, second, first},
		{second},
	} {
		if err := router.dispatch(order); err != nil {
			t.Fatalf("dispatch: %v", err)
		}
		want := make([][]string, len(order))
		for i, route := range order {
			want[i] = []string{"-j", "MT_" + route.spec.Name}
		}
		for _, hook := range routerHooks {
			if got := fake.GetRules(hook.table, "MT_ROUTING"); !reflect.DeepEqual(got, want) {
				t.Errorf("%s MT_ROUTING = %v, want %v", hook.table, got, want)
			}
		}
	}
}

func TestRouterDisableRemovesDispatchChain(t *testing.T) {
	router, fake := newTestRouter(t)

	if err := router.Disable(); err != nil {
		t.Fatalf("Disable: %v", err)
	}
	for _, hook := range routerHooks {
		want := [][]string{{"-j", "FOREIGN"}}
		if got := fake.GetRules(hook.table, hook.chain); !reflect.DeepEqual(got, want) {
			t.Errorf("%s %s = %v, want %v", hook.table, hook.chain, got, want)
		}
		if fake.ChainExists(hook.table, "MT_ROUTING") {
			t.Errorf("%s MT_ROUTING still exists", hook.table)
		}
	}
	if _, err := router.Prepare(nil, nil); err != ErrRouterDisabled {
		t.Errorf("Prepare after Disable = %v, want ErrRouterDisabled", err)
	}
}

func TestRouterPrepareRejectsDuplicateNames(t *testing.T) {
	router, _ := newTestRouter(t)

	_, err := router.Prepare([]RouteSpec{{Name: "a"}, {Name: "a"}}, nil)
	if err == nil {
		t.Fatal("Prepare with duplicate names succeeded")
	}
}

// countingIPTables считает вызовы iptables-restore и умеет их проваливать.
type countingIPTables struct {
	*iptables.FakeIPTables
	restores int
	fail     bool
}

func (c *countingIPTables) Restore(data []byte) error {
	c.restores++
	if c.fail {
		return errors.New("restore failed")
	}
	return c.FakeIPTables.Restore(data)
}

func newCountingIPTables(t *testing.T, proto iptables.Protocol) (*iptables.IPTables, *countingIPTables) {
	t.Helper()
	exec := &countingIPTables{FakeIPTables: iptables.NewFakeIPTables(proto)}
	ipt := iptables.NewIPTables(exec)
	for _, hook := range routerHooks {
		if err := ipt.RegisterChainPatch(hook.table, hook.chain); err != nil {
			t.Fatal(err)
		}
	}
	return ipt, exec
}

func TestRouterDispatchSkipsUnchangedOrder(t *testing.T) {
	ipt, exec := newCountingIPTables(t, iptables.ProtocolIPv4)
	router := (&Helper{ChainPrefix: "MT_", IPTables4: ipt}).Router("ROUTING")
	if err := router.Enable(); err != nil {
		t.Fatal(err)
	}
	a, b := testRoute(router.nh, "a"), testRoute(router.nh, "b")

	if err := router.dispatch([]*Route{a, b}); err != nil {
		t.Fatal(err)
	}
	before := exec.restores
	if err := router.dispatch([]*Route{a, b}); err != nil {
		t.Fatal(err)
	}
	if exec.restores != before {
		t.Errorf("unchanged order caused %d iptables-restore calls", exec.restores-before)
	}

	if err := router.dispatch([]*Route{b, a}); err != nil {
		t.Fatal(err)
	}
	if exec.restores == before {
		t.Error("changed order did not reach iptables")
	}
}

func TestRouterDispatchRetriesAfterFailure(t *testing.T) {
	ipt, exec := newCountingIPTables(t, iptables.ProtocolIPv4)
	router := (&Helper{ChainPrefix: "MT_", IPTables4: ipt}).Router("ROUTING")
	if err := router.Enable(); err != nil {
		t.Fatal(err)
	}
	a := testRoute(router.nh, "a")

	exec.fail = true
	if err := router.dispatch([]*Route{a}); err == nil {
		t.Fatal("dispatch error = nil, want restore failure")
	}
	exec.fail = false
	if err := router.dispatch([]*Route{a}); err != nil {
		t.Fatal(err)
	}
	if got := exec.GetRules("mangle", "MT_ROUTING"); !reflect.DeepEqual(got, [][]string{{"-j", "MT_a"}}) {
		t.Errorf("MT_ROUTING after retry = %v", got)
	}
}

func TestRouterEnableRollsBackAllFamilies(t *testing.T) {
	ipt4, exec4 := newCountingIPTables(t, iptables.ProtocolIPv4)
	ipt6, exec6 := newCountingIPTables(t, iptables.ProtocolIPv6)
	exec6.fail = true
	router := (&Helper{ChainPrefix: "MT_", IPTables4: ipt4, IPTables6: ipt6}).Router("ROUTING")

	if err := router.Enable(); err == nil {
		t.Fatal("Enable error = nil, want ipv6 failure")
	}
	for _, hook := range routerHooks {
		if exec4.ChainExists(hook.table, "MT_ROUTING") {
			t.Errorf("ipv4 %s MT_ROUTING left after failed Enable", hook.table)
		}
		if got := exec4.GetRules(hook.table, hook.chain); len(got) != 0 {
			t.Errorf("ipv4 %s %s = %v, want no jumps", hook.table, hook.chain, got)
		}
	}
}

// fakeSet — ipset без ядра. Как и IPSet, при swap объект сохраняет своё
// содержимое, а имя получает чужое.
type fakeSet struct {
	name       string
	content    map[IPv4Subnet]IPSetTimeout
	enabled    bool
	failEnable bool
	failSwap   bool
}

func newFakeSet(name string) *fakeSet {
	return &fakeSet{name: name, content: make(map[IPv4Subnet]IPSetTimeout)}
}

func (s *fakeSet) AddIPv4Subnet(subnet IPv4Subnet, ttl IPSetTimeout) error {
	s.content[subnet] = ttl
	return nil
}
func (s *fakeSet) AddIPv6Subnet(IPv6Subnet, IPSetTimeout) error { return nil }

func (s *fakeSet) enable() error {
	if s.failEnable {
		return errors.New("enable failed")
	}
	s.enabled = true
	return nil
}

func (s *fakeSet) disable() error {
	s.enabled = false
	return nil
}

func (s *fakeSet) swap(other routeSet) error {
	o := other.(*fakeSet)
	if s.failSwap || o.failSwap {
		return errors.New("swap failed")
	}
	s.name, o.name = o.name, s.name
	return nil
}

func (s *fakeSet) has(subnet IPv4Subnet) bool {
	_, ok := s.content[subnet]
	return ok
}

// fakeLink — привязка к интерфейсу без ядра: только состояние и управляемые сбои.
type fakeLink struct {
	chain       string
	enabled     bool
	failEnable  bool
	failDisable bool
	// onDisable вызывается при снятии.
	onDisable func()
}

func (l *fakeLink) chainName() string                       { return l.chain }
func (l *fakeLink) linkUpHook(netlink.LinkUpdate) error     { return nil }
func (l *fakeLink) addrChangeHook(netlink.AddrUpdate) error { return nil }

func (l *fakeLink) enable() error {
	if l.failEnable {
		return errors.New("enable failed")
	}
	l.enabled = true
	return nil
}

func (l *fakeLink) disable() error {
	if l.onDisable != nil {
		l.onDisable()
	}
	if l.failDisable {
		return errors.New("disable failed")
	}
	l.enabled = false
	return nil
}

// kernelRouter — Router на фейковом iptables с фейковыми маршрутами.
type kernelRouter struct {
	*Router
	fake *iptables.FakeIPTables
	// sets — ipset'ы по имени, под которым они созданы: <имя>-<слот>.
	sets map[string]*fakeSet
	// links — привязки по имени экземпляра: <имя>-<слот>.
	links map[string]*fakeLink
	// failEnable — интерфейсы, через которые маршрут не поднимается.
	failEnable map[string]bool
}

func newKernelRouter(t *testing.T) *kernelRouter {
	t.Helper()
	router, fake := newTestRouter(t)
	kr := &kernelRouter{Router: router, fake: fake, sets: map[string]*fakeSet{}, links: map[string]*fakeLink{}, failEnable: map[string]bool{}}
	router.newSet = func(name string) routeSet {
		s := newFakeSet(name)
		kr.sets[name] = s
		return s
	}
	router.newLink = func(name, iface string) routeLink {
		l := &fakeLink{chain: "MT_" + name, failEnable: kr.failEnable[iface]}
		kr.links[name] = l
		return l
	}
	return kr
}

func (kr *kernelRouter) apply(t *testing.T, specs ...RouteSpec) ([]*Route, error) {
	t.Helper()
	return kr.applyRefill(t, nil, specs...)
}

// applyRefill проводит обе фазы, а для не поднявшихся маршрутов оставляет
// прежние — как это делает routing.Table.
func (kr *kernelRouter) applyRefill(t *testing.T, refill map[string]bool, specs ...RouteSpec) ([]*Route, error) {
	t.Helper()
	routes, err := kr.Prepare(specs, refill)
	if routes == nil {
		return nil, err
	}
	active := kr.activeByName()
	var commit []*Route
	for i, route := range routes {
		if route == nil {
			route = active[specs[i].Name]
		}
		if route != nil {
			commit = append(commit, route)
		}
	}
	return routes, errors.Join(err, kr.Commit(commit))
}

// set возвращает ipset, который сейчас называется name (после swap имена
// обмениваются).
func (kr *kernelRouter) set(name string) *fakeSet {
	for _, s := range kr.sets {
		if s.name == name && s.enabled {
			return s
		}
	}
	return nil
}

func (kr *kernelRouter) dispatched() [][]string {
	return kr.fake.GetRules("mangle", "MT_ROUTING")
}

func jumps(chains ...string) [][]string {
	out := make([][]string, len(chains))
	for i, chain := range chains {
		out[i] = []string{"-j", chain}
	}
	return out
}

func TestRouterSwitchesInterfaceWithoutGap(t *testing.T) {
	kr := newKernelRouter(t)
	if _, err := kr.apply(t, RouteSpec{Name: "g", Interface: "wg0"}); err != nil {
		t.Fatal(err)
	}

	routes, err := kr.Prepare([]RouteSpec{{Name: "g", Interface: "wg1"}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	// Новый экземпляр поднят рядом, а трафик ещё идёт по старому.
	if !kr.links["g-0"].enabled || !kr.links["g-1"].enabled {
		t.Fatal("both instances must be up between Prepare and Commit")
	}
	if got := kr.dispatched(); !reflect.DeepEqual(got, jumps("MT_g-0")) {
		t.Fatalf("dispatcher changed before Commit: %v", got)
	}
	if routes[0].Spec().Interface != "wg1" {
		t.Fatalf("Prepare returned %+v, want the new instance", routes[0].Spec())
	}

	if err := kr.Commit(routes); err != nil {
		t.Fatal(err)
	}
	if got := kr.dispatched(); !reflect.DeepEqual(got, jumps("MT_g-1")) {
		t.Fatalf("dispatcher after Commit = %v, want the new instance", got)
	}
	if kr.links["g-0"].enabled || kr.sets["g-0"].enabled {
		t.Error("old instance was not removed after Commit")
	}
}

func TestRouterKeepsOldRouteWhenNewOneFails(t *testing.T) {
	kr := newKernelRouter(t)
	if _, err := kr.apply(t, RouteSpec{Name: "g", Interface: "wg0"}); err != nil {
		t.Fatal(err)
	}
	kr.failEnable["broken"] = true

	routes, err := kr.apply(t, RouteSpec{Name: "g", Interface: "broken"})
	var routeErr *RouteError
	if !errors.As(err, &routeErr) || routeErr.Name != "g" {
		t.Fatalf("error = %v, want RouteError for g", err)
	}
	if routes[0] != nil {
		t.Error("failed route returned as prepared")
	}
	if got := kr.dispatched(); !reflect.DeepEqual(got, jumps("MT_g-0")) {
		t.Fatalf("dispatcher = %v, want the old instance kept", got)
	}
	if !kr.links["g-0"].enabled || !kr.sets["g-0"].enabled {
		t.Fatal("old instance was removed although the new one failed")
	}
	if kr.sets["g-1"].enabled {
		t.Error("ipset of the failed instance was left")
	}

	// Откат к прежнему интерфейсу возвращает действующий маршрут без пересоздания.
	back, err := kr.apply(t, RouteSpec{Name: "g", Interface: "wg0"})
	if err != nil {
		t.Fatal(err)
	}
	if back[0].slot != 0 {
		t.Error("rollback recreated the route instead of reusing the active one")
	}
}

func TestRouterSurvivesFailedRemovalOfOldRoute(t *testing.T) {
	kr := newKernelRouter(t)
	if _, err := kr.apply(t, RouteSpec{Name: "g", Interface: "wg0"}); err != nil {
		t.Fatal(err)
	}
	kr.links["g-0"].failDisable = true

	if _, err := kr.apply(t, RouteSpec{Name: "g", Interface: "wg1"}); err != nil {
		t.Fatalf("failed removal of the old instance broke the switch: %v", err)
	}
	if got := kr.dispatched(); !reflect.DeepEqual(got, jumps("MT_g-1")) {
		t.Fatalf("dispatcher = %v, want the new instance", got)
	}
	if !kr.links["g-1"].enabled {
		t.Error("new instance is not up")
	}
}

func TestRouterDropsUncommittedPrepare(t *testing.T) {
	kr := newKernelRouter(t)
	if _, err := kr.apply(t, RouteSpec{Name: "g", Interface: "wg0"}); err != nil {
		t.Fatal(err)
	}

	if _, err := kr.Prepare([]RouteSpec{{Name: "g", Interface: "wg1"}}, nil); err != nil {
		t.Fatal(err)
	}
	// Commit не случился; следующий Prepare снимает брошенный экземпляр.
	if _, err := kr.Prepare([]RouteSpec{{Name: "g", Interface: "wg0"}}, nil); err != nil {
		t.Fatal(err)
	}
	if kr.links["g-1"].enabled || kr.sets["g-1"].enabled {
		t.Error("uncommitted instance was left up")
	}
	if !kr.links["g-0"].enabled || !kr.sets["g-0"].enabled {
		t.Error("active instance was removed")
	}
}

func TestRouterRemovesRoutesNotCommitted(t *testing.T) {
	kr := newKernelRouter(t)
	if _, err := kr.apply(t, RouteSpec{Name: "a", Interface: "wg0"}, RouteSpec{Name: "b", Interface: "wg0"}); err != nil {
		t.Fatal(err)
	}
	if _, err := kr.apply(t, RouteSpec{Name: "b", Interface: "wg0"}); err != nil {
		t.Fatal(err)
	}

	if kr.links["a-0"].enabled || kr.sets["a-0"].enabled {
		t.Error("removed route is still up")
	}
	if got := kr.dispatched(); !reflect.DeepEqual(got, jumps("MT_b-0")) {
		t.Fatalf("dispatcher = %v, want only b", got)
	}
}

var (
	oldHost = IPv4Subnet{Address: [4]byte{1, 1, 1, 1}}
	newHost = IPv4Subnet{Address: [4]byte{2, 2, 2, 2}}
)

// refillScenario поднимает маршрут g со старым содержимым и готовит замену с
// новым. Возвращает действующий и подготовленный маршруты.
func refillScenario(t *testing.T, kr *kernelRouter) (live, staged *Route) {
	t.Helper()
	spec := RouteSpec{Name: "g", Interface: "wg0"}
	routes, err := kr.apply(t, spec)
	if err != nil {
		t.Fatal(err)
	}
	live = routes[0]
	_ = live.AddIPv4Subnet(oldHost, nil)

	routes, err = kr.Prepare([]RouteSpec{spec}, map[string]bool{"g": true})
	if err != nil {
		t.Fatal(err)
	}
	staged = routes[0]
	if staged == live {
		t.Fatal("refill returned the live route")
	}
	_ = staged.AddIPv4Subnet(newHost, nil)
	return live, staged
}

func TestRouterRefillReplacesContentOnlyOnCommit(t *testing.T) {
	kr := newKernelRouter(t)
	_, staged := refillScenario(t, kr)

	// До Commit трафик видит только старое содержимое.
	if kr.sets["g-0"].has(newHost) || !kr.sets["g-0"].has(oldHost) {
		t.Fatal("live ipset changed before Commit")
	}
	before := kr.fake.GetRules("mangle", "MT_g-0")

	if err := kr.Commit([]*Route{staged}); err != nil {
		t.Fatal(err)
	}
	// Под именем, на которое ссылаются правила, теперь новое содержимое.
	live := kr.set("g-0")
	if live == nil || !live.has(newHost) || live.has(oldHost) {
		t.Fatal("ipset referenced by rules does not have the new content")
	}
	if staged.routeSet != live {
		t.Error("committed route does not own the live ipset")
	}
	if got := kr.dispatched(); !reflect.DeepEqual(got, jumps("MT_g-0")) {
		t.Errorf("dispatcher = %v, want the same chain", got)
	}
	if !kr.links["g-0"].enabled {
		t.Error("link shared with the committed route was removed")
	}
	if !reflect.DeepEqual(kr.fake.GetRules("mangle", "MT_g-0"), before) {
		t.Error("refill touched iptables rules of the route")
	}
	// Старое содержимое (теперь под вторым именем) удалено.
	for _, set := range kr.sets {
		if set != staged.routeSet && set.enabled {
			t.Errorf("ipset %q with old content was left", set.name)
		}
	}
}

func TestRouterRefillIsUndoneWhenDispatchFails(t *testing.T) {
	ipt, exec := newCountingIPTables(t, iptables.ProtocolIPv4)
	router := (&Helper{ChainPrefix: "MT_", IPTables4: ipt}).Router("ROUTING")
	if err := router.Enable(); err != nil {
		t.Fatal(err)
	}
	kr := &kernelRouter{Router: router, sets: map[string]*fakeSet{}, links: map[string]*fakeLink{}, failEnable: map[string]bool{}}
	router.newSet = func(name string) routeSet { s := newFakeSet(name); kr.sets[name] = s; return s }
	router.newLink = func(name, iface string) routeLink { l := &fakeLink{chain: "MT_" + name}; kr.links[name] = l; return l }

	live, staged := refillScenario(t, kr)
	other, err := router.Prepare([]RouteSpec{{Name: "g", Interface: "wg0"}, {Name: "h", Interface: "wg0"}}, map[string]bool{"g": true})
	if err != nil {
		t.Fatal(err)
	}
	staged = other[0]
	_ = staged.AddIPv4Subnet(newHost, nil)

	// Новый маршрут h меняет диспетчер, и это не удаётся.
	exec.fail = true
	if err := router.Commit(other); err == nil {
		t.Fatal("Commit error = nil, want dispatch failure")
	}
	if !live.routeSet.(*fakeSet).has(oldHost) || live.routeSet.(*fakeSet).name != "g-0" {
		t.Error("live ipset content was not restored after a failed Commit")
	}
	if staged.routeSet.(*fakeSet).has(oldHost) {
		t.Error("staged route got the live content")
	}
}

func TestRouterRefillIsUndoneWhenSwapFails(t *testing.T) {
	kr := newKernelRouter(t)
	a, err := kr.apply(t, RouteSpec{Name: "a", Interface: "wg0"}, RouteSpec{Name: "b", Interface: "wg0"})
	if err != nil {
		t.Fatal(err)
	}
	_ = a[0].AddIPv4Subnet(oldHost, nil)

	routes, err := kr.Prepare([]RouteSpec{{Name: "a", Interface: "wg0"}, {Name: "b", Interface: "wg0"}}, map[string]bool{"a": true, "b": true})
	if err != nil {
		t.Fatal(err)
	}
	kr.sets["b-1"].failSwap = true
	if err := kr.Commit(routes); err == nil {
		t.Fatal("Commit error = nil, want swap failure")
	}
	if !kr.sets["a-0"].has(oldHost) || kr.sets["a-0"].name != "a-0" {
		t.Error("ipset swapped before the failure was not swapped back")
	}
}

func TestRouterDisableRemovesDispatcherBeforeRoutes(t *testing.T) {
	kr := newKernelRouter(t)
	if _, err := kr.apply(t, RouteSpec{Name: "a", Interface: "wg0"}, RouteSpec{Name: "b", Interface: "wg1"}); err != nil {
		t.Fatal(err)
	}
	// Заменяемое содержимое ждёт Commit — его ipset тоже должен сняться.
	if _, err := kr.Prepare([]RouteSpec{{Name: "a", Interface: "wg0"}, {Name: "b", Interface: "wg1"}}, map[string]bool{"a": true}); err != nil {
		t.Fatal(err)
	}
	for name, link := range kr.links {
		link.onDisable = func() {
			for _, hook := range routerHooks {
				if kr.fake.ChainExists(hook.table, "MT_ROUTING") {
					t.Errorf("route %s removed while %s MT_ROUTING still refers to it", name, hook.table)
				}
			}
		}
	}

	if err := kr.Disable(); err != nil {
		t.Fatal(err)
	}
	for name, link := range kr.links {
		if link.enabled {
			t.Errorf("link %s is still up", name)
		}
	}
	for name, set := range kr.sets {
		if set.enabled {
			t.Errorf("ipset %s is still up", name)
		}
	}
}
