package magitrickle

import (
	"context"
	"errors"
	"sync"
	"testing"

	"magitrickle/app"
	"magitrickle/constant"
	"magitrickle/models"
	"magitrickle/routing"
	"magitrickle/utils/intID"
	"magitrickle/utils/netfilterTools"
	"magitrickle/utils/recordsCache"
)

func TestGroupAndSubscriptionIDsDoNotCollide(t *testing.T) {
	id := intID.ID{0xde, 0xad, 0xbe, 0xef}

	a := &App{}
	if err := a.AddSubscription(&models.Subscription{ID: id}); err != nil {
		t.Fatal(err)
	}
	if err := a.AddGroup(&models.Group{ID: id}); !errors.Is(err, app.ErrGroupIDConflict) {
		t.Fatalf("AddGroup with a subscription's ID = %v, want ErrGroupIDConflict", err)
	}

	a = &App{}
	if err := a.AddGroup(&models.Group{ID: id}); err != nil {
		t.Fatal(err)
	}
	if err := a.AddSubscription(&models.Subscription{ID: id}); !errors.Is(err, app.ErrSubscriptionConflict) {
		t.Fatalf("AddSubscription with a group's ID = %v, want ErrSubscriptionConflict", err)
	}
	if err := a.ReplaceSubscriptions([]*models.Subscription{{ID: id}}); !errors.Is(err, app.ErrSubscriptionConflict) {
		t.Fatalf("ReplaceSubscriptions with a group's ID = %v, want ErrSubscriptionConflict", err)
	}
}

func TestUpdateGroupDoesNotLoseConcurrentChanges(t *testing.T) {
	id := intID.ID{1, 2, 3, 4}
	a := &App{}
	if err := a.AddGroup(&models.Group{ID: id}); err != nil {
		t.Fatal(err)
	}

	const writers = 50
	var wg sync.WaitGroup
	for i := range writers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, err := a.UpdateGroup(id, func(current *models.Group) (*models.Group, error) {
				group := current.Clone()
				group.Rules = append(group.Rules, &models.Rule{ID: intID.ID{0, 0, 0, byte(i)}})
				return group, nil
			})
			if err != nil {
				t.Error(err)
			}
		}()
	}
	wg.Wait()

	if got := len(a.Group(id).Rules); got != writers {
		t.Fatalf("group has %d rules after %d concurrent additions", got, writers)
	}
}

// fakeRouter поднимает маршруты в памяти и отказывает маршрутам через
// интерфейсы из broken. routes — действующие маршруты.
type fakeRouter struct {
	broken   map[string]bool
	routes   map[string]*fakeRoute
	prepared map[string]*fakeRoute
}

type fakeRoute struct{ netfilterTools.RouteSpec }

func (fakeRoute) AddIPv4Subnet(netfilterTools.IPv4Subnet, netfilterTools.IPSetTimeout) error {
	return nil
}
func (fakeRoute) AddIPv6Subnet(netfilterTools.IPv6Subnet, netfilterTools.IPSetTimeout) error {
	return nil
}

func (r *fakeRouter) Prepare(specs []netfilterTools.RouteSpec, refill map[string]bool) ([]routing.Route, error) {
	r.prepared = make(map[string]*fakeRoute)
	out := make([]routing.Route, len(specs))
	var errs []error
	for i, spec := range specs {
		if r.broken[spec.Interface] {
			errs = append(errs, &netfilterTools.RouteError{Name: spec.Name, Err: errors.New("broken")})
			continue
		}
		route := r.routes[spec.Name]
		if route == nil || route.RouteSpec != spec || refill[spec.Name] {
			route = &fakeRoute{spec}
			r.prepared[spec.Name] = route
		}
		out[i] = route
	}
	return out, errors.Join(errs...)
}

func (r *fakeRouter) Commit(list []routing.Route) error {
	routes := make(map[string]*fakeRoute)
	for _, route := range list {
		route := route.(*fakeRoute)
		routes[route.Name] = route
	}
	r.routes = routes
	return nil
}

func newRunningApp(t *testing.T, router *fakeRouter) *App {
	t.Helper()
	a := &App{}
	a.routing.Store(routing.New(router, recordsCache.New()))
	return a
}

func TestBrokenGroupDoesNotBlockOtherGroups(t *testing.T) {
	router := &fakeRouter{broken: map[string]bool{}}
	a := newRunningApp(t, router)
	broken := &models.Group{ID: intID.ID{1}, Interface: "wg-broken", Enable: true}
	healthy := &models.Group{ID: intID.ID{2}, Interface: "wg0", Enable: true}
	if err := a.ReplaceGroups([]*models.Group{broken, healthy}); err != nil {
		t.Fatal(err)
	}

	// Интерфейс сломался после старта: изменение другой группы должно пройти.
	router.broken["wg-broken"] = true
	_, err := a.UpdateGroup(healthy.ID, func(current *models.Group) (*models.Group, error) {
		group := current.Clone()
		group.Name = "renamed"
		return group, nil
	})
	if err != nil {
		t.Fatalf("change of a healthy group blocked by a broken one: %v", err)
	}
	if a.Group(healthy.ID).Name != "renamed" {
		t.Error("change of a healthy group was rolled back")
	}
}

func TestFailedChangeIsRolledBack(t *testing.T) {
	router := &fakeRouter{broken: map[string]bool{"wg-broken": true}}
	a := newRunningApp(t, router)
	group := &models.Group{ID: intID.ID{1}, Interface: "wg0", Enable: true}
	if err := a.AddGroup(group); err != nil {
		t.Fatal(err)
	}

	_, err := a.UpdateGroup(group.ID, func(current *models.Group) (*models.Group, error) {
		moved := current.Clone()
		moved.Interface = "wg-broken"
		return moved, nil
	})
	if err == nil {
		t.Fatal("moving a group to a broken interface succeeded")
	}
	if got := a.Group(group.ID).Interface; got != "wg0" {
		t.Errorf("interface after failed change = %q, want wg0", got)
	}
	if route := router.routes[group.ID.String()]; route == nil || route.Interface != "wg0" {
		t.Errorf("routing was not restored: %+v", route)
	}
}

func TestUpdateGroupRejectsNilResult(t *testing.T) {
	id := intID.ID{1}
	a := &App{}
	if err := a.AddGroup(&models.Group{ID: id}); err != nil {
		t.Fatal(err)
	}
	_, err := a.UpdateGroup(id, func(*models.Group) (*models.Group, error) { return nil, nil })
	if err == nil {
		t.Fatal("UpdateGroup accepted a nil group")
	}
	if a.Group(id) == nil {
		t.Error("group was lost")
	}
}

func TestSubscriptionRuleIDsMustBeUnique(t *testing.T) {
	a := &App{}
	err := a.AddSubscription(&models.Subscription{ID: intID.ID{1}, Rules: []*models.SubscriptionRule{
		{ID: intID.ID{9}, Rule: "a.com"},
		{ID: intID.ID{9}, Rule: "b.com"},
	}})
	if !errors.Is(err, app.ErrRuleIDConflict) {
		t.Fatalf("AddSubscription with duplicate rule IDs = %v, want ErrRuleIDConflict", err)
	}
}

func TestSaveConfigRefusesUntilConfigIsLoaded(t *testing.T) {
	a := &App{}
	if err := a.SaveConfig(); !errors.Is(err, ErrConfigNotLoaded) {
		t.Fatalf("SaveConfig before a successful load = %v, want ErrConfigNotLoaded", err)
	}
}

func TestRepairConfigFixesHandEditedLeftovers(t *testing.T) {
	groups := []*models.Group{
		nil,
		{ID: intID.ID{1}, Rules: []*models.Rule{
			{ID: intID.ID{7}, Rule: "a.com"},
			nil,
			{ID: intID.ID{7}, Rule: "b.com"},
			{Rule: "c.com"},
		}},
	}
	subs := []*models.Subscription{
		{ID: intID.ID{2}, Rules: []*models.SubscriptionRule{nil, {Rule: "d.com"}, {Rule: "e.com"}}},
		nil,
	}

	groups, subs = repairConfig(groups, subs)

	if len(groups) != 1 || len(subs) != 1 {
		t.Fatalf("nil entries left: %d groups, %d subscriptions", len(groups), len(subs))
	}
	if len(groups[0].Rules) != 3 || len(subs[0].Rules) != 2 {
		t.Fatalf("nil rules left: %d group rules, %d subscription rules", len(groups[0].Rules), len(subs[0].Rules))
	}
	if err := validateSubscriptions(subs, nil); err != nil {
		t.Errorf("subscriptions still invalid: %v", err)
	}
	if err := validateGroups(groups, subs); err != nil {
		t.Errorf("groups still invalid: %v", err)
	}
	if groups[0].Rules[0].ID != (intID.ID{7}) {
		t.Error("first rule with a valid ID lost it")
	}
}

func TestRetryRoutingRecoversFailedRuleSets(t *testing.T) {
	router := &fakeRouter{broken: map[string]bool{}}
	a := newRunningApp(t, router)
	group := &models.Group{ID: intID.ID{1}, Interface: "wg0", Enable: true}
	if err := a.AddGroup(group); err != nil {
		t.Fatal(err)
	}
	table := a.routing.Load()

	// Маршрут пропал и не поднимается; следующее применение это замечает.
	router.broken["wg0"] = true
	delete(router.routes, group.ID.String())
	a.stateMu.Lock()
	_ = a.commitLocked(nil)
	a.stateMu.Unlock()
	if !table.Pending() {
		t.Fatal("Pending = false while the route cannot be brought up")
	}

	router.broken["wg0"] = false
	a.retryRouting()
	if table.Pending() {
		t.Error("retry did not recover the rule set")
	}
	if router.routes[group.ID.String()] == nil {
		t.Error("route was not brought up by the retry")
	}
}

func TestValidateAppConfigChecksKernelNamePrefixes(t *testing.T) {
	valid := func() *models.AppConfig {
		c := &models.AppConfig{}
		c.Netfilter.IPTables.ChainPrefix = "MT_"
		c.Netfilter.IPSet.TablePrefix = "mt_"
		return c
	}
	if err := validateAppConfig(valid()); err != nil {
		t.Fatalf("default-like prefixes rejected: %v", err)
	}

	for name, mutate := range map[string]func(*models.AppConfig){
		"empty chain prefix": func(c *models.AppConfig) { c.Netfilter.IPTables.ChainPrefix = "" },
		"empty ipset prefix": func(c *models.AppConfig) { c.Netfilter.IPSet.TablePrefix = "" },
		"long chain prefix":  func(c *models.AppConfig) { c.Netfilter.IPTables.ChainPrefix = "MAGITRICKLE_CHAINS_" },
		"long ipset prefix":  func(c *models.AppConfig) { c.Netfilter.IPSet.TablePrefix = "magitrickle_ipsets__" },
	} {
		c := valid()
		mutate(c)
		if err := validateAppConfig(c); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestDefaultConfigIsValid(t *testing.T) {
	defaults := constant.DefaultAppConfig
	if err := validateAppConfig(&defaults); err != nil {
		t.Fatalf("default config is invalid: %v", err)
	}
}

func TestStartRefusesWithoutLoadedConfig(t *testing.T) {
	a := &App{}
	loadErr := errors.New("broken yaml")
	a.configErr.Store(&loadErr)
	err := a.Start(context.Background())
	if !errors.Is(err, ErrConfigNotLoaded) || !errors.Is(err, loadErr) {
		t.Fatalf("Start without a loaded config = %v, want ErrConfigNotLoaded with the load error", err)
	}
}
