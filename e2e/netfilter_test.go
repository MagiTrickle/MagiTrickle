package e2e

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

const (
	firstMark  = 0x4d616769 // startMarkTableIndex 1298229097
	firstTable = 1298229097
)

// mtState snapshots everything MagiTrickle leaves in the kernel.
type mtState struct {
	filter4, mangle4, nat4 []string
	filter6, mangle6, nat6 []string
	sets                   []string
	rules4, rules6         string
}

func snapshot(t testing.TB) mtState {
	t.Helper()
	sets := rt.MustRun(t, "ipset list -n | grep '^mt_' || true")
	return mtState{
		filter4: mtRules(iptSave(t, "filter", false)),
		mangle4: mtRules(iptSave(t, "mangle", false)),
		nat4:    mtRules(iptSave(t, "nat", false)),
		filter6: mtRules(iptSave(t, "filter", true)),
		mangle6: mtRules(iptSave(t, "mangle", true)),
		nat6:    mtRules(iptSave(t, "nat", true)),
		sets:    strings.Fields(sets),
		rules4:  rt.MustRun(t, "ip rule | grep fwmark || true"),
		rules6:  rt.MustRun(t, "ip -6 rule | grep fwmark || true"),
	}
}

func (s mtState) empty() bool {
	return len(s.filter4)+len(s.mangle4)+len(s.nat4)+len(s.filter6)+len(s.mangle6)+len(s.nat6)+len(s.sets) == 0 &&
		strings.TrimSpace(s.rules4+s.rules6) == ""
}

func (s mtState) String() string {
	return fmt.Sprintf("filter4=%v\nmangle4=%v\nnat4=%v\nfilter6=%v\nmangle6=%v\nnat6=%v\nsets=%v\nrules4=%q rules6=%q",
		s.filter4, s.mangle4, s.nat4, s.filter6, s.mangle6, s.nat6, s.sets, s.rules4, s.rules6)
}

func count(lines []string, sub string) int {
	n := 0
	for _, l := range lines {
		if strings.Contains(l, sub) {
			n++
		}
	}
	return n
}

// jumps counts `-A <parent> -j <chain>` rules.
func jumps(lines []string, parent, chain string) int {
	n := 0
	for _, l := range lines {
		if strings.TrimSpace(l) == "-A "+parent+" -j "+chain {
			n++
		}
	}
	return n
}

func TestNetfilterLayout(t *testing.T) {
	rci.CreateWireguard(t, "Wireguard5", "E2E VPN")
	t.Cleanup(func() { rci.DeleteInterface(t, "Wireguard5") })
	StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "layout", Interface: "nwg5", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "chains", func() bool { return count(snapshot(t).mangle4, g.ID) > 0 })
	s := snapshot(t)
	chain := "MT_" + g.ID

	for _, tc := range []struct {
		name  string
		lines []string
		want  []string
	}{
		{"filter4", s.filter4, []string{
			"-A FORWARD -j " + chain,
			fmt.Sprintf("-A %s -o nwg5 -m set --match-set mt_%s_4 dst -j ACCEPT", chain, g.ID)}},
		{"mangle4", s.mangle4, []string{
			"-A PREROUTING -j " + chain,
			"-A " + chain + " -m conntrack --ctdir REPLY -j RETURN",
			fmt.Sprintf("-A %s -m set --match-set mt_%s_4 dst -j MARK --set-xmark 0x%x/0xffffffff", chain, g.ID, firstMark),
			fmt.Sprintf("-A %s -m set --match-set mt_%s_4 dst -j CONNMARK --save-mark", chain, g.ID)}},
		{"nat4", s.nat4, []string{
			"-A POSTROUTING -j " + chain,
			fmt.Sprintf("-A %s -m set --match-set mt_%s_4 dst -j MASQUERADE", chain, g.ID)}},
		{"filter6", s.filter6, []string{
			fmt.Sprintf("-A %s -o nwg5 -m set --match-set mt_%s_6 dst -j ACCEPT", chain, g.ID)}},
		{"mangle6", s.mangle6, []string{
			fmt.Sprintf("-A %s -m set --match-set mt_%s_6 dst -j MARK --set-xmark 0x%x/0xffffffff", chain, g.ID, firstMark)}},
		{"nat6", s.nat6, []string{
			fmt.Sprintf("-A %s -m set --match-set mt_%s_6 dst -j MASQUERADE", chain, g.ID)}},
	} {
		for _, w := range tc.want {
			if !containsLine(tc.lines, w) {
				t.Errorf("%s: missing %q in\n%s", tc.name, w, strings.Join(tc.lines, "\n"))
			}
		}
	}
	if len(s.sets) != 2 {
		t.Errorf("ipsets = %v, want the _4 and _6 set", s.sets)
	}
	if !strings.Contains(s.rules4, fmt.Sprintf("fwmark 0x%x lookup %d", firstMark, firstTable)) {
		t.Errorf("ip rule: %q", s.rules4)
	}
	if !strings.Contains(s.rules6, fmt.Sprintf("fwmark 0x%x lookup %d", firstMark, firstTable)) {
		t.Errorf("ip -6 rule: %q", s.rules6)
	}
	// Every jump is installed exactly once.
	for name, j := range map[string]struct {
		lines  []string
		parent string
	}{"filter4": {s.filter4, "FORWARD"}, "mangle4": {s.mangle4, "PREROUTING"}, "nat4": {s.nat4, "POSTROUTING"},
		"filter6": {s.filter6, "FORWARD"}, "mangle6": {s.mangle6, "PREROUTING"}, "nat6": {s.nat6, "POSTROUTING"}} {
		if n := jumps(j.lines, j.parent, chain); n != 1 {
			t.Errorf("%s: %d jumps to %s, want 1", name, n, chain)
		}
	}
}

func TestDNSRemapRules(t *testing.T) {
	StartDaemon(t, DaemonConfig{})
	var s mtState
	WaitFor(t, 5*time.Second, "DNSOR", func() bool { s = snapshot(t); return count(s.nat4, "MT_DNSOR") > 0 })

	for _, proto := range []string{"tcp", "udp"} {
		want := fmt.Sprintf("-A MT_DNSOR -d 192.168.1.1/32 -p %s -m %s --dport 53 -j DNAT --to-destination :3553", proto, proto)
		if !containsLine(s.nat4, want) {
			t.Errorf("missing %q in\n%s", want, strings.Join(s.nat4, "\n"))
		}
	}
	// The redirect must be the first rule of nat PREROUTING so ndm's own DNS redirects cannot pre-empt it.
	first := rt.MustRun(t, "iptables -t nat -S PREROUTING | sed -n 2p")
	if strings.TrimSpace(first) != "-A PREROUTING -j MT_DNSOR" {
		t.Errorf("first PREROUTING rule: %q", first)
	}
	if !containsLine(s.nat6, "-A PREROUTING -j MT_DNSOR") || !containsLine(s.nat6, "--dport 53 -j DNAT --to-destination :3553") {
		t.Errorf("IPv6 remap missing:\n%s", strings.Join(s.nat6, "\n"))
	}
	// Only the LAN-facing address of the router is redirected, not arbitrary DNS traffic.
	if containsLine(s.nat4, "-d 0.0.0.0/0") {
		t.Errorf("DNSOR matches every destination")
	}
}

func TestDisableRemap53(t *testing.T) {
	StartDaemon(t, DaemonConfig{DisableRemap53: true})
	api.CreateGroup(t, Group{Name: "g", Interface: "eth3", Enable: ptr(true)})
	time.Sleep(time.Second)
	s := snapshot(t)
	if count(s.nat4, "MT_DNSOR")+count(s.nat6, "MT_DNSOR") != 0 {
		t.Errorf("DNSOR present with disableRemap53:\n%s", s)
	}
}

func TestDisableIPv6(t *testing.T) {
	StartDaemon(t, DaemonConfig{DisableIPv6: true})
	g := api.CreateGroup(t, Group{Name: "g", Interface: "eth3", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "v4 chains", func() bool { return count(snapshot(t).mangle4, g.ID) > 0 })
	s := snapshot(t)
	if len(s.filter6)+len(s.mangle6)+len(s.nat6) != 0 {
		t.Errorf("ip6tables rules with disableIPv6:\n%s", s)
	}
	if strings.Contains(s.rules6, "fwmark") {
		t.Errorf("IPv6 ip rule with disableIPv6: %q", s.rules6)
	}
}

func TestDisableIPv4(t *testing.T) {
	StartDaemon(t, DaemonConfig{DisableIPv4: true})
	g := api.CreateGroup(t, Group{Name: "g", Interface: "eth3", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "v6 chains", func() bool { return count(snapshot(t).mangle6, g.ID) > 0 })
	s := snapshot(t)
	if len(s.filter4)+len(s.mangle4)+len(s.nat4) != 0 {
		t.Errorf("iptables rules with disableIPv4:\n%s", s)
	}
}

func TestGroupDisableEnableDelete(t *testing.T) {
	StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "toggle", Interface: "eth3", Enable: ptr(true),
		Rules: &[]Rule{{Name: "s", Type: "subnet", Rule: "192.0.2.10", Enable: true}}})
	waitMembers(t, ipsetName(g.ID, false), "192.0.2.10")

	gone := func() bool {
		s := snapshot(t)
		return count(s.mangle4, g.ID)+count(s.filter4, g.ID)+count(s.nat4, g.ID)+
			count(s.mangle6, g.ID)+count(s.filter6, g.ID)+count(s.nat6, g.ID) == 0 &&
			!strings.Contains(strings.Join(s.sets, " "), g.ID) && !strings.Contains(s.rules4, "fwmark")
	}
	present := func() bool {
		s := snapshot(t)
		return count(s.mangle4, g.ID) > 0 && count(s.nat6, g.ID) > 0 && len(s.sets) == 2
	}

	// Disable through PUT.
	g.Enable = ptr(false)
	api.OK(t, "PUT", "/groups/"+g.ID, g, nil)
	WaitFor(t, 5*time.Second, "group torn down", gone)

	// And back on: sets are repopulated from the rules.
	g.Enable = ptr(true)
	api.OK(t, "PUT", "/groups/"+g.ID, g, nil)
	WaitFor(t, 5*time.Second, "group set up again", present)
	waitMembers(t, ipsetName(g.ID, false), "192.0.2.10")

	// Changing the interface re-creates the filter rule for the new device.
	g.Interface = "br0"
	api.OK(t, "PUT", "/groups/"+g.ID, g, nil)
	WaitFor(t, 5*time.Second, "interface change", func() bool { return count(snapshot(t).filter4, "-o br0") == 1 })
	if s := snapshot(t); count(s.filter4, "-o eth3") != 0 {
		t.Errorf("old interface still in filter chain: %v", s.filter4)
	}

	api.OK(t, "DELETE", "/groups/"+g.ID, nil, nil)
	WaitFor(t, 5*time.Second, "group deleted", gone)
}

func TestMultipleGroupsGetDistinctMarks(t *testing.T) {
	StartDaemon(t, DaemonConfig{})
	g1 := api.CreateGroup(t, Group{Name: "one", Interface: "eth3", Enable: ptr(true)})
	g2 := api.CreateGroup(t, Group{Name: "two", Interface: "br0", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "both groups", func() bool {
		s := snapshot(t)
		return count(s.mangle4, g1.ID) > 0 && count(s.mangle4, g2.ID) > 0
	})
	s := snapshot(t)
	marks := map[string]string{}
	for _, l := range s.mangle4 {
		for _, g := range []Group{g1, g2} {
			if strings.Contains(l, "-A MT_"+g.ID+" ") && strings.Contains(l, "--set-xmark") {
				marks[g.ID] = l[strings.Index(l, "--set-xmark"):]
			}
		}
	}
	if len(marks) != 2 || marks[g1.ID] == marks[g2.ID] {
		t.Errorf("marks are not distinct: %v", marks)
	}
	if n := strings.Count(s.rules4, "fwmark"); n != 2 {
		t.Errorf("ip rules = %q, want 2 fwmark rules", s.rules4)
	}
}

// SIGTERM must remove every trace of the daemon from the router.
func TestGracefulShutdownCleansUp(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "clean", Interface: "eth3", Enable: ptr(true),
		Rules: &[]Rule{{Name: "s", Type: "subnet", Rule: "192.0.2.11", Enable: true}}})
	WaitFor(t, 5*time.Second, "chains", func() bool { return count(snapshot(t).mangle4, g.ID) > 0 })

	if err := d.Stop(15 * time.Second); err != nil {
		t.Fatal(err)
	}
	d.WaitLog("service stopped", time.Second)
	s := snapshot(t)
	if !s.empty() {
		t.Errorf("leftovers after SIGTERM:\n%s", s)
	}
	for _, f := range []string{pidFile, sockFile} {
		if _, err := rt.Run("test -e " + f); err == nil {
			t.Errorf("%s left after SIGTERM", f)
		}
	}
	// ndm's own tables must be intact.
	if !containsLine(strings.Split(iptSave(t, "nat", false), "\n"), "_NDM_DNAT") {
		t.Errorf("ndm chains disappeared from nat")
	}
}

// After SIGKILL the next start must not duplicate or leak rules.
func TestStartupCleansStaleState(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "stale", Interface: "eth3", Enable: ptr(true)})
	api.Save(t)
	WaitFor(t, 5*time.Second, "chains", func() bool { return count(snapshot(t).mangle4, g.ID) > 0 })

	d.Signal("KILL")
	WaitFor(t, 5*time.Second, "daemon killed", d.Exited)
	if s := snapshot(t); s.empty() {
		t.Fatal("SIGKILL cleaned up, so the test proves nothing")
	}
	rt.MustRun(t, "rm -f "+pidFile+" "+sockFile)

	d2 := spawnDaemon(t)
	_ = d2
	WaitFor(t, 5*time.Second, "chains again", func() bool { return count(snapshot(t).mangle4, g.ID) > 0 })
	time.Sleep(time.Second)
	s := snapshot(t)
	for name, j := range map[string]struct {
		lines  []string
		parent string
	}{"mangle4": {s.mangle4, "PREROUTING"}, "filter4": {s.filter4, "FORWARD"}, "nat4": {s.nat4, "POSTROUTING"}} {
		if n := jumps(j.lines, j.parent, "MT_"+g.ID); n != 1 {
			t.Errorf("%s: %d jumps to MT_%s after restart:\n%s", name, n, g.ID, strings.Join(j.lines, "\n"))
		}
	}
	if len(s.sets) != 2 {
		t.Errorf("ipsets after restart: %v", s.sets)
	}
	if n := strings.Count(s.rules4, "fwmark"); n != 1 || !strings.Contains(s.rules4, fmt.Sprintf("0x%x", firstMark)) {
		t.Errorf("ip rules after restart: %q (stale rule not removed, or mark moved)", s.rules4)
	}
	if out := rt.MustRun(t, "ip route show table all | grep -c 'table 1298229098' || true"); strings.TrimSpace(out) != "0" {
		t.Errorf("stale routing table left behind: %s", out)
	}
	if n := count(s.nat4, "-A PREROUTING -j MT_DNSOR"); n != 1 {
		t.Errorf("DNSOR jumps = %d", n)
	}
}

func TestIPSetMembersExpire(t *testing.T) {
	StartDaemon(t, DaemonConfig{AdditionalTTL: "1s"})
	g := api.CreateGroup(t, Group{Name: "exp", Interface: "eth3", Enable: ptr(true),
		Rules: &[]Rule{{Name: "r", Type: "domain", Rule: "exp.nf.test", Enable: true}}})
	SetZone(t, ZoneRecord{Name: "exp.nf.test", Type: "A", Value: "203.0.113.70", TTL: 1})
	dig(t, "exp.nf.test", "A")
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.70")
	WaitFor(t, 10*time.Second, "member to expire", func() bool { return len(ipsetMembers(t, ipsetName(g.ID, false))) == 0 })
}

// A restarted daemon must remove iptables entries it no longer owns: leftovers of a killed instance,
// of a group that is gone from the config, and hand-made chains under its prefix.
func TestRestartRemovesStaleIPTablesEntries(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "doomed", Interface: "eth3", Enable: ptr(true)})
	chain := "MT_" + g.ID
	WaitFor(t, 5*time.Second, "chains", func() bool { return jumps(snapshot(t).mangle4, "PREROUTING", chain) == 1 })

	// Config on disk has no groups, so after the crash nobody will ever clean "doomed" except the startup sweep.
	d.Signal("KILL")
	WaitFor(t, 5*time.Second, "daemon killed", d.Exited)
	rt.MustRun(t, "rm -f "+pidFile+" "+sockFile)

	// Plus a foreign leftover under the prefix, in every table of both families.
	for _, tool := range []string{"iptables", "ip6tables"} {
		for _, table := range []string{"filter", "mangle", "nat"} {
			parent := map[string]string{"filter": "FORWARD", "mangle": "PREROUTING", "nat": "POSTROUTING"}[table]
			rt.MustRun(t, fmt.Sprintf("%[1]s -t %[2]s -N MT_deadbeef && %[1]s -t %[2]s -A MT_deadbeef -j RETURN && %[1]s -t %[2]s -A %[3]s -j MT_deadbeef", tool, table, parent))
		}
	}
	before := snapshot(t)
	for _, c := range []string{chain, "MT_deadbeef"} {
		if count(before.mangle4, c) == 0 || count(before.nat6, c) == 0 {
			t.Fatalf("setup failed, %s not present:\n%s", c, before)
		}
	}

	spawnDaemon(t)
	WaitFor(t, 10*time.Second, "stale entries swept", func() bool {
		s := snapshot(t)
		for _, c := range []string{chain, "MT_deadbeef"} {
			if count(s.filter4, c)+count(s.mangle4, c)+count(s.nat4, c)+count(s.filter6, c)+count(s.mangle6, c)+count(s.nat6, c) != 0 {
				return false
			}
		}
		return true
	})
	// Only the daemon's own redirect remains, and ndm's chains are untouched.
	s := snapshot(t)
	if n := jumps(s.nat4, "PREROUTING", "MT_DNSOR"); n != 1 {
		t.Errorf("DNSOR jumps = %d", n)
	}
	if !strings.Contains(iptSave(t, "nat", false), "_NDM_DNAT") {
		t.Error("ndm chains damaged by the sweep")
	}
}
