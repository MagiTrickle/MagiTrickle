package e2e

import (
	"slices"
	"testing"
	"time"
)

func sortedMembers(t testing.TB, set string) []string {
	t.Helper()
	m := ipsetMembers(t, set)
	slices.Sort(m)
	return m
}

func waitMembers(t testing.TB, set string, want ...string) {
	t.Helper()
	slices.Sort(want)
	var got []string
	WaitFor(t, 6*time.Second, "ipset "+set+" == "+join(want), func() bool {
		got = sortedMembers(t, set)
		return slices.Equal(got, want)
	})
}

func join(s []string) string {
	out := "["
	for i, v := range s {
		if i > 0 {
			out += " "
		}
		out += v
	}
	return out + "]"
}

// Every rule type must put the addresses of matching names, and only those, into the group's set.
func TestRuleTypesMatching(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "types", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "domain", Type: "domain", Rule: "exact.rt.test", Enable: true},
		{Name: "namespace", Type: "namespace", Rule: "ns.rt.test", Enable: true},
		{Name: "wildcard1", Type: "wildcard", Rule: "*.wc.rt.test", Enable: true},
		{Name: "wildcard2", Type: "wildcard", Rule: "q?x.rt.test", Enable: true},
		{Name: "regex", Type: "regex", Rule: `^re[0-9]+\.rt\.test$`, Enable: true},
		{Name: "disabled", Type: "domain", Rule: "off.rt.test", Enable: false},
	}})
	set := ipsetName(g.ID, false)

	SetZone(t,
		ZoneRecord{Name: "exact.rt.test", Type: "A", Value: "203.0.113.1"},
		ZoneRecord{Name: "sub.exact.rt.test", Type: "A", Value: "203.0.113.2"},
		ZoneRecord{Name: "ns.rt.test", Type: "A", Value: "203.0.113.3"},
		ZoneRecord{Name: "a.b.ns.rt.test", Type: "A", Value: "203.0.113.4"},
		ZoneRecord{Name: "xns.rt.test", Type: "A", Value: "203.0.113.5"},
		ZoneRecord{Name: "host.wc.rt.test", Type: "A", Value: "203.0.113.6"},
		ZoneRecord{Name: "wc.rt.test", Type: "A", Value: "203.0.113.7"},
		ZoneRecord{Name: "qzx.rt.test", Type: "A", Value: "203.0.113.8"},
		ZoneRecord{Name: "qzzx.rt.test", Type: "A", Value: "203.0.113.9"},
		ZoneRecord{Name: "re42.rt.test", Type: "A", Value: "203.0.113.10"},
		ZoneRecord{Name: "reab.rt.test", Type: "A", Value: "203.0.113.11"},
		ZoneRecord{Name: "off.rt.test", Type: "A", Value: "203.0.113.12"},
		ZoneRecord{Name: "other.test", Type: "A", Value: "203.0.113.13"},
	)
	for _, n := range []string{
		"exact.rt.test", "sub.exact.rt.test", "ns.rt.test", "a.b.ns.rt.test", "xns.rt.test",
		"host.wc.rt.test", "wc.rt.test", "qzx.rt.test", "qzzx.rt.test", "re42.rt.test", "reab.rt.test",
		"off.rt.test", "other.test",
	} {
		if len(query(t, n, "A")) != 1 {
			t.Fatalf("no answer for %s", n)
		}
	}
	waitMembers(t, set,
		"203.0.113.1",  // domain
		"203.0.113.3",  // namespace apex
		"203.0.113.4",  // namespace sub-sub
		"203.0.113.6",  // wildcard *
		"203.0.113.8",  // wildcard ?
		"203.0.113.10", // regex
	)
}

func TestSubnetRules(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "subnets", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "cidr", Type: "subnet", Rule: "198.51.100.0/24", Enable: true},
		{Name: "single", Type: "subnet", Rule: "192.0.2.5", Enable: true},
		{Name: "off", Type: "subnet", Rule: "192.0.2.77", Enable: false},
		{Name: "v6", Type: "subnet6", Rule: "2001:db8:1::/48", Enable: true},
		{Name: "v6single", Type: "subnet6", Rule: "2001:db8:2::5", Enable: true},
	}})
	waitMembers(t, ipsetName(g.ID, false), "198.51.100.0/24", "192.0.2.5")
	waitMembers(t, ipsetName(g.ID, true), "2001:db8:1::/48", "2001:db8:2::5")

	// Editing the rule list re-syncs the sets.
	var rules struct {
		Rules []Rule `json:"rules"`
	}
	api.OK(t, "GET", "/groups/"+g.ID+"/rules", nil, &rules)
	var cidr Rule
	for _, r := range rules.Rules {
		if r.Name == "cidr" {
			cidr = r
		}
	}
	cidr.Rule = "198.51.101.0/24"
	api.OK(t, "PUT", "/groups/"+g.ID+"/rules/"+cidr.ID, cidr, nil)
	waitMembers(t, ipsetName(g.ID, false), "198.51.101.0/24", "192.0.2.5")
}

func TestInvalidSubnetRulesAreIgnored(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "bad", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "junk", Type: "subnet", Rule: "not-an-ip", Enable: true},
		{Name: "v6-as-v4", Type: "subnet", Rule: "2001:db8::1", Enable: true},
		{Name: "good", Type: "subnet", Rule: "192.0.2.9", Enable: true},
	}})
	waitMembers(t, ipsetName(g.ID, false), "192.0.2.9")
	if len(ipsetMembers(t, ipsetName(g.ID, true))) != 0 {
		t.Errorf("v6 set is not empty: %v", ipsetMembers(t, ipsetName(g.ID, true)))
	}
	if d.Exited() {
		t.Fatal("daemon died")
	}
}

func TestRegexRuleWithBrokenPatternDoesNotCrash(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "badre", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "broken", Type: "regex", Rule: `([unclosed`, Enable: true},
		{Name: "ok", Type: "domain", Rule: "ok.rt.test", Enable: true},
	}})
	SetZone(t,
		ZoneRecord{Name: "ok.rt.test", Type: "A", Value: "203.0.113.20"},
		ZoneRecord{Name: "x.rt.test", Type: "A", Value: "203.0.113.21"})
	query(t, "x.rt.test", "A")
	query(t, "ok.rt.test", "A")
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.20")
	if d.Exited() {
		t.Fatal("daemon died")
	}
}

// The same name in two groups must land in both sets.
func TestSameNameInTwoGroups(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	rules := &[]Rule{{Name: "r", Type: "namespace", Rule: "both.rt.test", Enable: true}}
	g1 := api.CreateGroup(t, Group{Name: "one", Interface: "eth3", Enable: ptr(true), Rules: rules})
	g2 := api.CreateGroup(t, Group{Name: "two", Interface: "br0", Enable: ptr(true), Rules: rules})
	SetZone(t, ZoneRecord{Name: "www.both.rt.test", Type: "A", Value: "203.0.113.30"})
	query(t, "www.both.rt.test", "A")
	waitMembers(t, ipsetName(g1.ID, false), "203.0.113.30")
	waitMembers(t, ipsetName(g2.ID, false), "203.0.113.30")
}

func TestDisabledGroupGetsNothing(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "off", Interface: "eth3", Enable: ptr(false), Rules: &[]Rule{
		{Name: "r", Type: "namespace", Rule: "off.rt.test", Enable: true}}})
	SetZone(t, ZoneRecord{Name: "www.off.rt.test", Type: "A", Value: "203.0.113.31"})
	query(t, "www.off.rt.test", "A")
	time.Sleep(time.Second)
	if ipsetExists(t, ipsetName(g.ID, false)) {
		t.Errorf("ipset created for a disabled group")
	}
	if rules := mtRules(iptSave(t, "mangle", false)); containsLine(rules, g.ID) {
		t.Errorf("iptables rules created for a disabled group: %v", rules)
	}
}
