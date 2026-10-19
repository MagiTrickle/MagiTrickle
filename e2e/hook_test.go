package e2e

import (
	"fmt"
	"net/http"
	"regexp"
	"sort"
	"strings"
	"testing"
	"time"
)

// The Keenetic firmware runs every executable in /opt/etc/ndm/netfilter.d after it rebuilds its netfilter
// tables, with $type (iptables|ip6tables) and $table (filter|mangle|nat|...) in the environment.
// https://github.com/ndmsystems/packages/wiki/Opkg-Component#ndmnetfilterd
var hookEventRe = regexp.MustCompile(`received netfilter\.d event.*table=(\w+).*type=(\w+)`)

func hookEvents(d *Daemon) []string {
	var ev []string
	for _, m := range hookEventRe.FindAllStringSubmatch(d.Output(), -1) {
		ev = append(ev, m[2]+"/"+m[1])
	}
	return ev
}

func uniq(in []string) []string {
	seen := map[string]bool{}
	var out []string
	for _, s := range in {
		if !seen[s] {
			seen[s] = true
			out = append(out, s)
		}
	}
	sort.Strings(out)
	return out
}

// jumpDel removes our jump rule from the parent chain, imitating ndm rebuilding the table.
func jumpDel(t testing.TB, tool, table, parent, chain string) {
	t.Helper()
	rt.MustRun(t, fmt.Sprintf("%s -t %s -D %s -j %s", tool, table, parent, chain))
}

func jumpPresent(t testing.TB, tool, table, parent, chain string) bool {
	t.Helper()
	_, err := rt.Run(fmt.Sprintf("%s -t %s -C %s -j %s", tool, table, parent, chain))
	return err == nil
}

func runHook(t testing.TB, typ, table string) {
	t.Helper()
	rt.MustRun(t, fmt.Sprintf("type=%s table=%s %s", typ, table, hookFile))
}

type jump struct{ tool, typ, table, parent string }

var allJumps = []jump{
	{"iptables", "iptables", "filter", "FORWARD"},
	{"iptables", "iptables", "mangle", "PREROUTING"},
	{"iptables", "iptables", "nat", "POSTROUTING"},
	{"ip6tables", "ip6tables", "filter", "FORWARD"},
	{"ip6tables", "ip6tables", "mangle", "PREROUTING"},
	{"ip6tables", "ip6tables", "nat", "POSTROUTING"},
}

func TestHookScriptRestoresRules(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "hook", Interface: "eth3", Enable: ptr(true)})
	chain := "MT_" + g.ID
	WaitFor(t, 5*time.Second, "chains", func() bool { return jumpPresent(t, "ip6tables", "nat", "POSTROUTING", chain) })

	for _, j := range allJumps {
		t.Run(j.typ+"/"+j.table, func(t *testing.T) {
			jumpDel(t, j.tool, j.table, j.parent, chain)
			if jumpPresent(t, j.tool, j.table, j.parent, chain) {
				t.Fatal("jump still present after delete")
			}
			before := len(hookEvents(d))
			runHook(t, j.typ, j.table)
			WaitFor(t, 5*time.Second, "jump restored", func() bool { return jumpPresent(t, j.tool, j.table, j.parent, chain) })
			if got := hookEvents(d); len(got) != before+1 || got[len(got)-1] != j.typ+"/"+j.table {
				t.Errorf("daemon events %v, want one new %s/%s", got, j.typ, j.table)
			}
			// restoring must not duplicate anything
			if n := jumps(strings.Split(iptSaveTool(t, j.tool, j.table), "\n"), j.parent, chain); n != 1 {
				t.Errorf("%d jumps after restore", n)
			}
		})
	}
}

func iptSaveTool(t testing.TB, tool, table string) string {
	t.Helper()
	return rt.MustRun(t, fmt.Sprintf("%s-save -t %s", tool, table))
}

// The DNS redirect has to come back as the first rule, ahead of ndm's own redirects.
func TestHookRestoresDNSRedirectFirst(t *testing.T) {
	StartDaemon(t, DaemonConfig{})
	WaitFor(t, 5*time.Second, "DNSOR", func() bool { return jumpPresent(t, "iptables", "nat", "PREROUTING", "MT_DNSOR") })

	jumpDel(t, "iptables", "nat", "PREROUTING", "MT_DNSOR")
	rt.MustRun(t, "iptables -t nat -I PREROUTING 1 -j _NDM_DNAT") // some ndm rule now occupies the first slot
	t.Cleanup(func() { rt.Run("iptables -t nat -D PREROUTING -j _NDM_DNAT") })

	runHook(t, "iptables", "nat")
	WaitFor(t, 5*time.Second, "DNSOR restored", func() bool { return jumpPresent(t, "iptables", "nat", "PREROUTING", "MT_DNSOR") })
	first := strings.TrimSpace(rt.MustRun(t, "iptables -t nat -S PREROUTING | sed -n 2p"))
	if first != "-A PREROUTING -j MT_DNSOR" {
		t.Errorf("first PREROUTING rule = %q", first)
	}
}

func TestHookScriptWithoutDaemonIsHarmless(t *testing.T) {
	StopAll(t)
	rt.Run("rm -f " + sockFile)
	start := time.Now()
	out, err := rt.Run("type=iptables table=mangle " + hookFile)
	if err != nil {
		t.Fatalf("hook failed without daemon: %v\n%s", err, out)
	}
	if time.Since(start) > 3*time.Second {
		t.Errorf("hook took %s without a daemon", time.Since(start))
	}
	// A stale socket file that nobody listens on must not hang ndm either.
	rt.MustRun(t, "rm -f /opt/tmp/dead.sock; (socat UNIX-LISTEN:/opt/tmp/dead.sock /dev/null &) ; sleep 1; killall socat; true")
}

func TestHookEndpointValidation(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	post := func(body string) int {
		req, _ := http.NewRequest("POST", mtBase+"/system/hooks/netfilterd", strings.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		return resp.StatusCode
	}
	if c := post(`{"type":"iptables","table":"nat"}`); c != 200 {
		t.Errorf("valid hook = %d", c)
	}
	if c := post(`{broken`); c != 400 {
		t.Errorf("malformed hook = %d", c)
	}
	if c := post(``); c != 400 {
		t.Errorf("empty hook = %d", c)
	}
	// Unknown tables are accepted: the commit is global, the daemon just re-applies its rules.
	if c := post(`{"type":"iptables","table":"raw"}`); c != 200 {
		t.Errorf("unknown table = %d", c)
	}
	if d.Exited() {
		t.Fatal("daemon died")
	}
}

// ndm hands the hook to us for real when the Internet connection is switched in the Keenetic UI.
func TestNDMFiresHookOnConnectionToggle(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "ndm", Interface: "eth3", Enable: ptr(true)})
	chain := "MT_" + g.ID
	WaitFor(t, 5*time.Second, "chains", func() bool { return jumpPresent(t, "iptables", "mangle", "PREROUTING", chain) })
	before := len(hookEvents(d))

	// Simulate the table being rebuilt underneath us, then let ndm do its thing.
	for _, j := range allJumps[:3] {
		jumpDel(t, j.tool, j.table, j.parent, chain)
	}
	rci.SetInterface(t, "GigabitEthernet1", false)
	WaitFor(t, 30*time.Second, "hook events after link down", func() bool { return len(hookEvents(d)) > before })
	rci.SetInterface(t, "GigabitEthernet1", true)

	WaitFor(t, 30*time.Second, "all IPv4 jumps restored", func() bool {
		for _, j := range allJumps[:3] {
			if !jumpPresent(t, j.tool, j.table, j.parent, chain) {
				return false
			}
		}
		return true
	})
	t.Logf("hook events seen: %v", uniq(hookEvents(d)[before:]))
	if d.Exited() {
		t.Fatal("daemon died")
	}
}

func TestNDMHookStorm(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "storm", Interface: "eth3", Enable: ptr(true)})
	chain := "MT_" + g.ID
	WaitFor(t, 5*time.Second, "chains", func() bool { return jumpPresent(t, "iptables", "mangle", "PREROUTING", chain) })

	for i := 0; i < 3; i++ {
		rci.SetInterface(t, "GigabitEthernet1", false)
		rci.SetInterface(t, "GigabitEthernet1", true)
	}
	time.Sleep(5 * time.Second)
	for _, j := range allJumps {
		if n := jumps(strings.Split(iptSaveTool(t, j.tool, j.table), "\n"), j.parent, chain); n != 1 {
			t.Errorf("%s/%s: %d jumps after storm", j.typ, j.table, n)
		}
	}
	if d.Exited() {
		t.Fatal("daemon died")
	}
	if code, _, err := api.Do("GET", "/groups", nil); err != nil || code != 200 {
		t.Errorf("API after storm: %v %d", err, code)
	}
}

// Another trigger that touches the nat table: the guest network coming up.
func TestNDMFiresNATHookOnGuestNetworkToggle(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "guest", Interface: "eth3", Enable: ptr(true)})
	chain := "MT_" + g.ID
	WaitFor(t, 5*time.Second, "chains", func() bool { return jumpPresent(t, "iptables", "nat", "POSTROUTING", chain) })
	t.Cleanup(func() { rci.SetInterface(t, "Bridge1", false) })

	before := len(hookEvents(d))
	jumpDel(t, "iptables", "nat", "POSTROUTING", chain)
	rci.SetInterface(t, "Bridge1", true)
	WaitFor(t, 30*time.Second, "nat hook event", func() bool {
		for _, e := range hookEvents(d)[before:] {
			if e == "iptables/nat" {
				return true
			}
		}
		return false
	})
	WaitFor(t, 10*time.Second, "nat jump restored", func() bool { return jumpPresent(t, "iptables", "nat", "POSTROUTING", chain) })
}
