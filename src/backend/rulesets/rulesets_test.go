package rulesets

import (
	"net/netip"
	"reflect"
	"testing"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

func keys(list []RuleSet) []string {
	out := make([]string, len(list))
	for i, rs := range list {
		out[i] = rs.Key()
	}
	return out
}

func group(id intID.ID, name, iface string, rules []*models.Rule) RuleSet {
	rs, _ := FromGroup(&models.Group{ID: id, Name: name, Interface: iface, Enable: true, Rules: rules})
	return rs
}

func TestBuildPutsSubscriptionsBeforeGroups(t *testing.T) {
	groups := []*models.Group{
		{ID: intID.ID{0, 0, 0, 1}, Interface: "wg0", Enable: true},
		{ID: intID.ID{0, 0, 0, 2}, Interface: "wg1", Enable: true},
	}
	subs := []*models.Subscription{
		{ID: intID.ID{0, 0, 0, 4}, Interface: "wg0", Enable: true},
		{ID: intID.ID{0, 0, 0, 3}, Interface: "wg1", Enable: true},
	}

	got := keys(Build(subs, groups))
	want := []string{"00000004", "00000003", "00000001", "00000002"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("Build order = %v, want %v", got, want)
	}
}

func TestBuildSkipsInactive(t *testing.T) {
	groups := []*models.Group{
		{ID: intID.ID{1}, Interface: "wg0", Enable: false},
		{ID: intID.ID{2}, Interface: "", Enable: true},
		nil,
		{ID: intID.ID{3}, Interface: "wg0", Enable: true},
	}
	subs := []*models.Subscription{
		{ID: intID.ID{4}, Interface: "wg0", Enable: false},
		{ID: intID.ID{5}, Interface: "", Enable: true},
		nil,
	}

	got := keys(Build(subs, groups))
	want := []string{"03000000"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("Build = %v, want %v", got, want)
	}
}

func TestFromGroupTakesOnlyEnabledRules(t *testing.T) {
	enabled := &models.Rule{ID: intID.ID{1}, Type: models.RuleTypeDomain, Rule: "a.com", Enable: true}
	group := &models.Group{
		ID:        intID.ID{9},
		Name:      "Group",
		Interface: "wg0",
		Enable:    true,
		Rules: []*models.Rule{
			enabled,
			{ID: intID.ID{2}, Type: models.RuleTypeDomain, Rule: "b.com", Enable: false},
		},
	}

	rs, ok := FromGroup(group)
	if !ok {
		t.Fatal("FromGroup ok = false")
	}
	if rs.ID != group.ID || rs.Name != "Group" || rs.Interface != "wg0" {
		t.Fatalf("unexpected rule set %+v", rs)
	}
	if !rs.Matches([]string{"a.com"}) {
		t.Error("enabled rule does not match")
	}
	if rs.Matches([]string{"b.com"}) {
		t.Error("disabled rule matches")
	}

	enabled.Rule = "changed.com"
	if !rs.Matches([]string{"a.com"}) || rs.Matches([]string{"changed.com"}) {
		t.Error("changing the model affected the snapshot")
	}
}

func TestFromSubscription(t *testing.T) {
	sub := &models.Subscription{
		ID:        intID.ID{0xaa, 0xbb, 0xcc, 0xdd},
		Interface: "wg0",
		Enable:    true,
		Rules: []*models.SubscriptionRule{
			{ID: intID.ID{1}, Type: models.RuleTypeNamespace, Rule: "example.com", Enable: true},
			{ID: intID.ID{2}, Type: models.RuleTypeDomain, Rule: "off.com", Enable: false},
		},
	}

	rs, ok := FromSubscription(sub)
	if !ok {
		t.Fatal("FromSubscription ok = false")
	}
	if rs.Key() != "aabbccdd" {
		t.Fatalf("Key = %q, want aabbccdd", rs.Key())
	}
	if !rs.Matches([]string{"www.example.com"}) || rs.Matches([]string{"off.com"}) {
		t.Error("subscription rules are not applied as expected")
	}
}

func TestSubnetsAreSeparatedFromDomainRules(t *testing.T) {
	rs := group(intID.ID{1}, "", "wg0", []*models.Rule{
		{Type: models.RuleTypeSubnet, Rule: "10.1.2.3/8", Enable: true},
		{Type: models.RuleTypeSubnet, Rule: "192.168.0.1", Enable: true},
		{Type: models.RuleTypeSubnet6, Rule: "2001:db8::/32", Enable: true},
		{Type: models.RuleTypeSubnet, Rule: "2001:db8::/32", Enable: true}, // не та семья
		{Type: models.RuleTypeSubnet6, Rule: "10.0.0.0/8", Enable: true},   // не та семья
		{Type: models.RuleTypeSubnet, Rule: "garbage", Enable: true},
		{Type: models.RuleTypeSubnet, Rule: "172.16.0.0/12", Enable: false},
	})

	want := []netip.Prefix{
		netip.MustParsePrefix("10.0.0.0/8"),
		netip.MustParsePrefix("192.168.0.1/32"),
		netip.MustParsePrefix("2001:db8::/32"),
	}
	if !reflect.DeepEqual(rs.Subnets, want) {
		t.Fatalf("Subnets = %v, want %v", rs.Subnets, want)
	}
	if rs.Matches([]string{"10.1.2.3", "192.168.0.1"}) {
		t.Error("subnet rule matched a domain name")
	}
}

func TestSameRules(t *testing.T) {
	rules := func(pattern string) []*models.Rule {
		return []*models.Rule{
			{Type: models.RuleTypeNamespace, Rule: "example.com", Enable: true},
			{Type: models.RuleTypeRegEx, Rule: pattern, Enable: true},
			{Type: models.RuleTypeSubnet, Rule: "10.0.0.0/8", Enable: true},
		}
	}

	a := group(intID.ID{1}, "a", "wg0", rules("^a"))
	if !a.SameRules(group(intID.ID{1}, "b", "wg1", rules("^a"))) {
		t.Error("equal rules reported as different")
	}
	if a.SameRules(group(intID.ID{1}, "a", "wg0", rules("^b"))) {
		t.Error("different regex reported as same")
	}
}

func TestSameRulesIgnoresOrderOfIndexedRules(t *testing.T) {
	a := group(intID.ID{1}, "", "wg0", []*models.Rule{
		{Type: models.RuleTypeDomain, Rule: "a.com", Enable: true},
		{Type: models.RuleTypeNamespace, Rule: "b.com", Enable: true},
		{Type: models.RuleTypeWildcard, Rule: "*.c.com", Enable: true},
	})
	b := group(intID.ID{1}, "", "wg0", []*models.Rule{
		{Type: models.RuleTypeWildcard, Rule: "*.c.com", Enable: true},
		{Type: models.RuleTypeNamespace, Rule: "b.com", Enable: true},
		{Type: models.RuleTypeDomain, Rule: "a.com", Enable: true},
		{Type: models.RuleTypeDomain, Rule: "a.com", Enable: true},
	})

	if !a.SameRules(b) {
		t.Error("reordered and duplicated rules reported as different")
	}
}

func TestSameRulesDistinguishesWildcardDot(t *testing.T) {
	wildcard := func(pattern string) RuleSet {
		return group(intID.ID{1}, "", "wg0", []*models.Rule{
			{Type: models.RuleTypeWildcard, Rule: pattern, Enable: true},
		})
	}

	if wildcard("*example.com").SameRules(wildcard("*.example.com")) {
		t.Error(`"*example.com" and "*.example.com" reported as same`)
	}
}

func TestBuilderReusesRuleSetsOfUnchangedModels(t *testing.T) {
	sub := &models.Subscription{ID: intID.ID{1}, Interface: "wg0", Enable: true, Rules: []*models.SubscriptionRule{
		{Type: models.RuleTypeNamespace, Rule: "example.com", Enable: true},
	}}
	group := &models.Group{ID: intID.ID{2}, Interface: "wg0", Enable: true}

	var b Builder
	first := b.Build([]*models.Subscription{sub}, []*models.Group{group})

	renamed := group.Clone()
	renamed.Name = "renamed"
	second := b.Build([]*models.Subscription{sub}, []*models.Group{renamed})

	if first[0].Domains != second[0].Domains {
		t.Error("subscription rule set was rebuilt although the model did not change")
	}
	if second[1].Name != "renamed" {
		t.Error("replaced group was not rebuilt")
	}
	if !first[0].SameRules(second[0]) || !first[1].SameRules(second[1]) {
		t.Error("SameRules = false for unchanged rules")
	}
}

func TestBuilderReusesRuleSetOfClonedModelWithSameRules(t *testing.T) {
	sub := &models.Subscription{ID: intID.ID{1}, Interface: "wg0", Enable: true, Rules: []*models.SubscriptionRule{
		{Type: models.RuleTypeNamespace, Rule: "example.com", Enable: true},
	}}

	var b Builder
	first := b.Build([]*models.Subscription{sub}, nil)

	checked := sub.Clone()
	checked.LastCheck = 42
	checked.Interface = "wg1"
	second := b.Build([]*models.Subscription{checked}, nil)
	if first[0].Domains != second[0].Domains {
		t.Error("rules were parsed again although only LastCheck and Interface changed")
	}
	if second[0].Interface != "wg1" {
		t.Errorf("Interface = %q, want the new wg1", second[0].Interface)
	}

	refreshed := checked.Clone()
	refreshed.Rules = []*models.SubscriptionRule{{Type: models.RuleTypeDomain, Rule: "other.org", Enable: true}}
	third := b.Build([]*models.Subscription{refreshed}, nil)
	if third[0].Domains == second[0].Domains || !third[0].Matches([]string{"other.org"}) {
		t.Error("changed rules were not rebuilt")
	}
}
