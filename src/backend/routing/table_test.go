package routing

import (
	"errors"
	"fmt"
	"maps"
	"net"
	"reflect"
	"testing"

	"magitrickle/models"
	"magitrickle/rulesets"
	"magitrickle/utils/intID"
	"magitrickle/utils/netfilterTools"
	"magitrickle/utils/recordsCache"
)

type fakeRoute struct {
	name    string
	v4      map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout
	v6      map[netfilterTools.IPv6Subnet]netfilterTools.IPSetTimeout
	adds    int
	failAdd bool
	// beforeAdd вызывается перед каждой записью IPv4-адреса.
	beforeAdd func()
}

func newFakeRoute(name string) *fakeRoute {
	return &fakeRoute{
		name: name,
		v4:   make(map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout),
		v6:   make(map[netfilterTools.IPv6Subnet]netfilterTools.IPSetTimeout),
	}
}

func (r *fakeRoute) AddIPv4Subnet(s netfilterTools.IPv4Subnet, ttl netfilterTools.IPSetTimeout) error {
	if r.beforeAdd != nil {
		r.beforeAdd()
	}
	r.adds++
	if r.failAdd {
		return errors.New("add failed")
	}
	r.v4[s] = ttl
	return nil
}

func (r *fakeRoute) AddIPv6Subnet(s netfilterTools.IPv6Subnet, ttl netfilterTools.IPSetTimeout) error {
	r.adds++
	r.v6[s] = ttl
	return nil
}

// fakeRouter повторяет контракт netfilterTools.Router: маршрут с той же
// спецификацией переживает Prepare, если его не просят наполнить заново;
// иначе выдаётся новый пустой, который становится действующим только после
// Commit. routes — действующие маршруты.
type fakeRouter struct {
	routes    map[string]*fakeRoute
	specs     map[string]netfilterTools.RouteSpec
	prepared  map[string]*fakeRoute
	pending   map[string]netfilterTools.RouteSpec
	applied   [][]netfilterTools.RouteSpec
	fail      map[string]bool
	fatal     error
	commitErr error
	onCommit  func()
	// onPrepare вызывается для каждого нового маршрута.
	onPrepare func(*fakeRoute)
}

func newFakeRouter() *fakeRouter {
	return &fakeRouter{
		routes: make(map[string]*fakeRoute),
		specs:  make(map[string]netfilterTools.RouteSpec),
		fail:   make(map[string]bool),
	}
}

func (r *fakeRouter) Prepare(specs []netfilterTools.RouteSpec, refill map[string]bool) ([]Route, error) {
	r.applied = append(r.applied, specs)
	if r.fatal != nil {
		return nil, r.fatal
	}
	r.prepared = make(map[string]*fakeRoute)
	r.pending = make(map[string]netfilterTools.RouteSpec)
	out := make([]Route, len(specs))
	var errs []error
	for i, spec := range specs {
		if r.fail[spec.Name] {
			errs = append(errs, &netfilterTools.RouteError{Name: spec.Name, Err: errors.New("failed")})
			continue
		}
		route := r.routes[spec.Name]
		if route == nil || r.specs[spec.Name] != spec || refill[spec.Name] {
			route = newFakeRoute(spec.Name)
			if r.onPrepare != nil {
				r.onPrepare(route)
			}
			r.prepared[spec.Name], r.pending[spec.Name] = route, spec
		}
		out[i] = route
	}
	return out, errors.Join(errs...)
}

func (r *fakeRouter) Commit(list []Route) error {
	if r.onCommit != nil {
		r.onCommit()
	}
	if r.commitErr != nil {
		return r.commitErr
	}
	routes := make(map[string]*fakeRoute)
	specs := make(map[string]netfilterTools.RouteSpec)
	for _, route := range list {
		route := route.(*fakeRoute)
		switch {
		case r.prepared[route.name] == route:
			specs[route.name] = r.pending[route.name]
		case r.routes[route.name] == route:
			specs[route.name] = r.specs[route.name]
		default:
			return fmt.Errorf("route %s is neither active nor prepared", route.name)
		}
		routes[route.name] = route
	}
	r.routes, r.specs = routes, specs
	return nil
}

func newRecords() *recordsCache.Records {
	records := recordsCache.New()
	records.AddAddress("example.com", net.IPv4(1, 1, 1, 1).To4(), 300)
	records.AddAddress("other.org", net.IPv4(2, 2, 2, 2).To4(), 300)
	return records
}

func ruleSet(id byte, iface string, rules ...*models.Rule) rulesets.RuleSet {
	rs, _ := rulesets.FromGroup(&models.Group{ID: intID.ID{0, 0, 0, id}, Interface: iface, Enable: true, Rules: rules})
	return rs
}

func domain(name string) *models.Rule {
	return &models.Rule{Type: models.RuleTypeDomain, Rule: name, Enable: true}
}

func host4(a, b, c, d byte) netfilterTools.IPv4Subnet {
	return netfilterTools.IPv4Subnet{Address: [4]byte{a, b, c, d}}
}

func TestApplyPassesRuleSetOrderToRouter(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())

	err := table.Apply([]rulesets.RuleSet{
		ruleSet(9, "wg0"),
		ruleSet(1, "wg1"),
		ruleSet(2, "blackhole"),
	})
	if err != nil {
		t.Fatal(err)
	}

	want := []netfilterTools.RouteSpec{
		{Name: "00000009", Interface: "wg0"},
		{Name: "00000001", Interface: "wg1"},
		{Name: "00000002", Interface: "blackhole"},
	}
	if got := router.applied[0]; !reflect.DeepEqual(got, want) {
		t.Fatalf("router specs = %v, want %v", got, want)
	}
}

func TestApplyFillsNewRoutesFromCache(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())

	err := table.Apply([]rulesets.RuleSet{
		ruleSet(1, "wg0",
			domain("example.com"),
			&models.Rule{Type: models.RuleTypeSubnet, Rule: "10.0.0.1/8", Enable: true},
			&models.Rule{Type: models.RuleTypeSubnet6, Rule: "2001:db8::/32", Enable: true},
		),
	})
	if err != nil {
		t.Fatal(err)
	}

	route := router.routes["00000001"]
	if ttl, ok := route.v4[host4(1, 1, 1, 1)]; !ok || ttl == nil || *ttl == 0 || *ttl > 300 {
		t.Errorf("example.com address missing or has wrong ttl: %v", route.v4)
	}
	if _, ok := route.v4[host4(2, 2, 2, 2)]; ok {
		t.Error("unrelated domain was routed")
	}
	if ttl, ok := route.v4[netfilterTools.IPv4Subnet{Address: [4]byte{10}, CIDR: 8}]; !ok || ttl != nil {
		t.Errorf("subnet rule missing or not permanent: %v", route.v4)
	}
	v6 := netfilterTools.IPv6Subnet{Address: [16]byte{0x20, 0x01, 0x0d, 0xb8}, CIDR: 32}
	if ttl, ok := route.v6[v6]; !ok || ttl != nil {
		t.Errorf("subnet6 rule missing or not permanent: %v", route.v6)
	}
}

func TestApplyResyncsOnlyChangedRuleSets(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())

	sub := ruleSet(9, "wg0", domain("other.org"))
	group := ruleSet(1, "wg1", domain("example.com"))
	if err := table.Apply([]rulesets.RuleSet{sub, group}); err != nil {
		t.Fatal(err)
	}
	subRoute, groupRoute := router.routes["00000009"], router.routes["00000001"]
	subAdds := subRoute.adds

	// Группа поменяла правила, подписка — нет. Правила пересобраны заново,
	// как это делает rulesets.Build.
	sub = ruleSet(9, "wg0", domain("other.org"))
	group = ruleSet(1, "wg1", domain("other.org"))
	if err := table.Apply([]rulesets.RuleSet{sub, group}); err != nil {
		t.Fatal(err)
	}

	if router.routes["00000009"] != subRoute || subRoute.adds != subAdds {
		t.Error("route of an unchanged subscription was touched")
	}
	refilled := router.routes["00000001"]
	if refilled == groupRoute {
		t.Fatal("changed group was filled in place")
	}
	if _, ok := refilled.v4[host4(1, 1, 1, 1)]; ok {
		t.Error("address of a removed rule is still routed")
	}
	if _, ok := refilled.v4[host4(2, 2, 2, 2)]; !ok {
		t.Error("address of a new rule is not routed")
	}
}

func TestApplyRefillsRecreatedRoute(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())

	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg1", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}

	if _, ok := router.routes["00000001"].v4[host4(1, 1, 1, 1)]; !ok {
		t.Error("route recreated for a new interface was not refilled")
	}
}

func TestApplySkipsFailedRoutes(t *testing.T) {
	router := newFakeRouter()
	router.fail["00000001"] = true
	table := New(router, newRecords())

	err := table.Apply([]rulesets.RuleSet{
		ruleSet(1, "wg0", domain("example.com")),
		ruleSet(2, "wg0", domain("example.com")),
	})
	if err == nil {
		t.Fatal("Apply error = nil, want router error")
	}

	table.AddAddresses([]string{"example.com"}, []Address{{IP: net.IPv4(3, 3, 3, 3).To4(), TTL: 60}})
	if _, ok := router.routes["00000002"].v4[host4(3, 3, 3, 3)]; !ok {
		t.Error("healthy rule set stopped routing after another one failed")
	}
}

func TestAddAddressesRoutesMatchingRuleSetsByAlias(t *testing.T) {
	router := newFakeRouter()
	table := New(router, recordsCache.New())

	err := table.Apply([]rulesets.RuleSet{
		ruleSet(9, "wg0", &models.Rule{Type: models.RuleTypeNamespace, Rule: "example.com", Enable: true}),
		ruleSet(1, "wg1", domain("other.org")),
	})
	if err != nil {
		t.Fatal(err)
	}

	table.AddAddresses(
		[]string{"cdn.net", "www.example.com"},
		[]Address{{IP: net.IPv4(4, 4, 4, 4).To4(), TTL: 60}, {IP: net.ParseIP("2001:db8::1"), TTL: 60}},
	)

	sub := router.routes["00000009"]
	if ttl := sub.v4[host4(4, 4, 4, 4)]; ttl == nil || *ttl != 60 {
		t.Errorf("ipv4 address not routed through the matching rule set: %v", sub.v4)
	}
	if len(sub.v6) != 1 {
		t.Errorf("ipv6 address not routed through the matching rule set: %v", sub.v6)
	}
	if group := router.routes["00000001"]; len(group.v4)+len(group.v6) != 0 {
		t.Errorf("address routed through a rule set that does not match: %v %v", group.v4, group.v6)
	}
}

func TestApplyDoesNotBlockOnUnchangedBrokenRuleSet(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())
	broken := ruleSet(1, "wg0", domain("example.com"))
	healthy := ruleSet(2, "wg0", domain("example.com"))

	router.fail["00000001"] = true
	if err := table.Apply([]rulesets.RuleSet{broken, healthy}); err == nil {
		t.Fatal("first Apply of a broken rule set succeeded")
	}

	// Меняется только healthy; broken по-прежнему не поднимается.
	healthy = ruleSet(2, "wg0", domain("other.org"))
	if err := table.Apply([]rulesets.RuleSet{broken, healthy}); err != nil {
		t.Fatalf("unrelated change blocked by a still broken rule set: %v", err)
	}
	if _, ok := router.routes["00000002"].v4[host4(2, 2, 2, 2)]; !ok {
		t.Error("unrelated change was not applied")
	}

	// Сломанный набор повторяется и поднимается, когда роутер оживает.
	router.fail["00000001"] = false
	if err := table.Apply([]rulesets.RuleSet{broken, healthy}); err != nil {
		t.Fatal(err)
	}
	if _, ok := router.routes["00000001"].v4[host4(1, 1, 1, 1)]; !ok {
		t.Error("recovered rule set was not filled")
	}
}

func TestApplyReportsSyncFailures(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0")}); err != nil {
		t.Fatal(err)
	}
	live := router.routes["00000001"]
	failFill := true
	router.onPrepare = func(route *fakeRoute) { route.failAdd = failFill }

	changed := ruleSet(1, "wg0", domain("example.com"))
	if err := table.Apply([]rulesets.RuleSet{changed}); err == nil {
		t.Fatal("Apply hid a failure to fill the route")
	}
	if router.routes["00000001"] != live {
		t.Fatal("partially filled route replaced the working one")
	}

	// Тот же набор ещё раз: наполняется заново и подменяет прежний.
	failFill = false
	if err := table.Apply([]rulesets.RuleSet{changed}); err != nil {
		t.Fatal(err)
	}
	if _, ok := router.routes["00000001"].v4[host4(1, 1, 1, 1)]; !ok {
		t.Error("rule set with a failed fill was not refilled on the next Apply")
	}
}

func TestApplyKeepsEntriesWhenRouterAppliesNothing(t *testing.T) {
	router := newFakeRouter()
	table := New(router, recordsCache.New())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}

	router.fatal = errors.New("dispatch failed")
	if err := table.Apply(nil); err == nil {
		t.Fatal("Apply error = nil, want router failure")
	}

	table.AddAddresses([]string{"example.com"}, []Address{{IP: net.IPv4(5, 5, 5, 5).To4(), TTL: 60}})
	if _, ok := router.routes["00000001"].v4[host4(5, 5, 5, 5)]; !ok {
		t.Error("DNS answers stopped reaching routes after a failed Apply")
	}
}

func TestApplyFillsNewRouteBeforeCommit(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}

	var filledAtCommit bool
	router.onCommit = func() {
		_, filledAtCommit = router.prepared["00000001"].v4[host4(1, 1, 1, 1)]
	}
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg1", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}
	if !filledAtCommit {
		t.Error("traffic was switched to the new route before it was filled")
	}
}

func TestApplyKeepsOldRouteWhenNewOneFails(t *testing.T) {
	router := newFakeRouter()
	table := New(router, recordsCache.New())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}
	old := router.routes["00000001"]

	router.fail["00000001"] = true
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg1", domain("example.com"))}); err == nil {
		t.Fatal("Apply error = nil, want route failure")
	}
	if router.routes["00000001"] != old {
		t.Fatal("old route was not kept by Commit")
	}

	table.AddAddresses([]string{"example.com"}, []Address{{IP: net.IPv4(6, 6, 6, 6).To4(), TTL: 60}})
	if _, ok := old.v4[host4(6, 6, 6, 6)]; !ok {
		t.Error("DNS answers stopped reaching the route that still carries traffic")
	}
}

func TestSyncKeepsFresherTTLFromConcurrentDNSAnswer(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0")}); err != nil {
		t.Fatal(err)
	}
	// DNS-ответ с длинным TTL приходит во время наполнения, до записи из кэша.
	var route *fakeRoute
	dnsDone := false
	router.onPrepare = func(r *fakeRoute) {
		route = r
		r.beforeAdd = func() {
			if dnsDone {
				return
			}
			dnsDone = true
			table.AddAddresses([]string{"example.com"}, []Address{{IP: net.IPv4(1, 1, 1, 1).To4(), TTL: 9999}})
		}
	}
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}

	if !dnsDone {
		t.Fatal("scenario did not trigger")
	}
	ttl := route.v4[host4(1, 1, 1, 1)]
	if ttl == nil {
		t.Fatal("address is missing or permanent, want TTL 9999")
	}
	if *ttl != 9999 {
		t.Fatalf("TTL = %d, want the fresher 9999 from DNS", *ttl)
	}
}

func TestApplyCommitFailureIsFatalAndKeepsOldEntries(t *testing.T) {
	router := newFakeRouter()
	table := New(router, recordsCache.New())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}
	old := router.routes["00000001"]

	router.commitErr = errors.New("dispatch failed")
	err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg1", domain("example.com"))})
	var applyErr *ApplyError
	if !errors.As(err, &applyErr) || applyErr.Fatal == nil {
		t.Fatalf("error = %v, want a fatal ApplyError", err)
	}

	table.AddAddresses([]string{"example.com"}, []Address{{IP: net.IPv4(7, 7, 7, 7).To4(), TTL: 60}})
	if _, ok := old.v4[host4(7, 7, 7, 7)]; !ok {
		t.Error("DNS answers go to routes that were never switched on")
	}
}

func TestApplyRuleSetFailureIsNotFatal(t *testing.T) {
	router := newFakeRouter()
	router.fail["00000001"] = true
	table := New(router, recordsCache.New())

	err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0"), ruleSet(2, "wg0")})
	var applyErr *ApplyError
	if !errors.As(err, &applyErr) {
		t.Fatalf("error = %v, want ApplyError", err)
	}
	if applyErr.Fatal != nil {
		t.Errorf("failure of one rule set reported as fatal: %v", applyErr.Fatal)
	}
	if _, ok := applyErr.RuleSets["00000001"]; !ok || len(applyErr.RuleSets) != 1 {
		t.Errorf("RuleSets = %v, want only 00000001", applyErr.RuleSets)
	}
}

func TestDNSAnswerBeforeCommitReachesRouteCarryingTraffic(t *testing.T) {
	router := newFakeRouter()
	table := New(router, recordsCache.New())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}
	old := router.routes["00000001"]

	router.onCommit = func() {
		table.AddAddresses([]string{"example.com"}, []Address{{IP: net.IPv4(8, 8, 8, 8).To4(), TTL: 60}})
	}
	router.commitErr = errors.New("dispatch failed")
	_ = table.Apply([]rulesets.RuleSet{ruleSet(1, "wg1", domain("example.com"))})

	if _, ok := old.v4[host4(8, 8, 8, 8)]; !ok {
		t.Error("address from a DNS answer before Commit missed the route that still carries traffic")
	}
}

func TestJournalReplayKeepsAnswerArrivingDuringReplay(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0")}); err != nil {
		t.Fatal(err)
	}
	// 1-я запись (из кэша) — приходит ответ с TTL 9999;
	// 2-я запись (проигрывание 9999) — приходит ответ с TTL 20000.
	var route *fakeRoute
	writes, inDNS := 0, false
	router.onPrepare = func(r *fakeRoute) {
		route = r
		r.beforeAdd = func() {
			if inDNS {
				return
			}
			writes++
			var ttl uint32
			switch writes {
			case 1:
				ttl = 9999
			case 2:
				ttl = 20000
			default:
				return
			}
			inDNS = true
			table.AddAddresses([]string{"example.com"}, []Address{{IP: net.IPv4(1, 1, 1, 1).To4(), TTL: ttl}})
			inDNS = false
		}
	}
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}

	if writes < 2 {
		t.Fatal("scenario did not trigger")
	}
	ttl := route.v4[host4(1, 1, 1, 1)]
	if ttl == nil || *ttl != 20000 {
		got := "permanent or missing"
		if ttl != nil {
			got = fmt.Sprint(*ttl)
		}
		t.Fatalf("TTL = %s, want the freshest 20000", got)
	}
}

func TestPendingReportsRuleSetsToRetry(t *testing.T) {
	router := newFakeRouter()
	router.fail["00000001"] = true
	table := New(router, recordsCache.New())
	sets := []rulesets.RuleSet{ruleSet(1, "wg0")}

	_ = table.Apply(sets)
	if !table.Pending() {
		t.Fatal("Pending = false after a rule set failed")
	}

	router.fail["00000001"] = false
	if err := table.Apply(sets); err != nil {
		t.Fatal(err)
	}
	if table.Pending() {
		t.Error("Pending = true after everything was applied")
	}
}

func ttlOf(t *testing.T, route *fakeRoute, s netfilterTools.IPv4Subnet) netfilterTools.IPSetTimeout {
	t.Helper()
	ttl, ok := route.v4[s]
	if !ok {
		t.Fatalf("%s is not in the route", s)
	}
	return ttl
}

func TestChangedRulesDoNotTouchLiveRouteBeforeCommit(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}
	live := router.routes["00000001"]
	adds := live.adds

	var liveAtCommit map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout
	router.onCommit = func() { liveAtCommit = maps.Clone(live.v4) }
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("other.org"))}); err != nil {
		t.Fatal(err)
	}
	if live.adds != adds || !reflect.DeepEqual(liveAtCommit, map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout{host4(1, 1, 1, 1): live.v4[host4(1, 1, 1, 1)]}) {
		t.Errorf("live route was changed before Commit: %v", liveAtCommit)
	}
	if _, ok := router.routes["00000001"].v4[host4(2, 2, 2, 2)]; !ok {
		t.Error("new rules were not applied")
	}
}

func TestCommitFailureLeavesLiveRouteInSync(t *testing.T) {
	router := newFakeRouter()
	table := New(router, recordsCache.New())
	old := ruleSet(1, "wg0", domain("example.com"))
	if err := table.Apply([]rulesets.RuleSet{old}); err != nil {
		t.Fatal(err)
	}
	live := router.routes["00000001"]

	// Правила изменились, а Commit не удался (например, из-за нового маршрута в том же изменении).
	router.commitErr = errors.New("dispatch failed")
	changed := ruleSet(1, "wg0", &models.Rule{Type: models.RuleTypeSubnet, Rule: "10.0.0.0/8", Enable: true})
	if err := table.Apply([]rulesets.RuleSet{changed, ruleSet(2, "wg0")}); err == nil {
		t.Fatal("Apply error = nil, want commit failure")
	}

	// Вызывающий откатывает изменение.
	router.commitErr = nil
	if err := table.Apply([]rulesets.RuleSet{old}); err != nil {
		t.Fatal(err)
	}
	if router.routes["00000001"] != live {
		t.Fatal("rollback replaced the live route")
	}
	if _, ok := live.v4[netfilterTools.IPv4Subnet{Address: [4]byte{10}, CIDR: 8}]; ok {
		t.Error("after rollback the live route contains a subnet from the rejected rules")
	}
}

func TestDNSDuringSwitchFollowsRulesOfEachRoute(t *testing.T) {
	router := newFakeRouter()
	table := New(router, recordsCache.New())
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("old.com"))}); err != nil {
		t.Fatal(err)
	}
	live := router.routes["00000001"]

	oldIP, newIP := net.IPv4(1, 1, 1, 1).To4(), net.IPv4(2, 2, 2, 2).To4()
	var staged *fakeRoute
	router.onPrepare = func(r *fakeRoute) { staged = r }
	router.onCommit = func() {
		table.AddAddresses([]string{"old.com"}, []Address{{IP: oldIP, TTL: 60}})
		table.AddAddresses([]string{"new.com"}, []Address{{IP: newIP, TTL: 60}})
	}
	router.commitErr = errors.New("dispatch failed")
	_ = table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("new.com"))})

	if _, ok := live.v4[host4(1, 1, 1, 1)]; !ok {
		t.Error("answer matching the rules of the live route missed it")
	}
	if _, ok := live.v4[host4(2, 2, 2, 2)]; ok {
		t.Error("answer matching only the new rules reached the live route")
	}
	if _, ok := staged.v4[host4(1, 1, 1, 1)]; ok {
		t.Error("answer matching only the old rules reached the new route")
	}
	if _, ok := staged.v4[host4(2, 2, 2, 2)]; !ok {
		t.Error("answer matching the new rules missed the new route")
	}
}

func TestRemovedSubnetRuleLeavesOnlyTemporaryEntry(t *testing.T) {
	router := newFakeRouter()
	table := New(router, newRecords())
	subnetAndDomain := ruleSet(1, "wg0",
		&models.Rule{Type: models.RuleTypeSubnet, Rule: "1.1.1.1", Enable: true},
		domain("example.com"))
	if err := table.Apply([]rulesets.RuleSet{subnetAndDomain}); err != nil {
		t.Fatal(err)
	}
	if ttl := ttlOf(t, router.routes["00000001"], host4(1, 1, 1, 1)); ttl != nil {
		t.Fatalf("host from a subnet rule is not permanent: %d", *ttl)
	}

	// subnet-правило удалили, домен остался.
	if err := table.Apply([]rulesets.RuleSet{ruleSet(1, "wg0", domain("example.com"))}); err != nil {
		t.Fatal(err)
	}
	if ttl := ttlOf(t, router.routes["00000001"], host4(1, 1, 1, 1)); ttl == nil {
		t.Error("entry stayed permanent after its subnet rule was removed")
	}
}

func TestDNSDoesNotReplacePermanentHost(t *testing.T) {
	router := newFakeRouter()
	table := New(router, recordsCache.New())
	rs := ruleSet(1, "wg0",
		&models.Rule{Type: models.RuleTypeSubnet, Rule: "1.1.1.1", Enable: true},
		domain("example.com"))
	if err := table.Apply([]rulesets.RuleSet{rs}); err != nil {
		t.Fatal(err)
	}

	table.AddAddresses([]string{"example.com"}, []Address{{IP: net.IPv4(1, 1, 1, 1).To4(), TTL: 60}})
	if ttl := ttlOf(t, router.routes["00000001"], host4(1, 1, 1, 1)); ttl != nil {
		t.Errorf("DNS answer made a permanent host temporary: %d", *ttl)
	}
}
