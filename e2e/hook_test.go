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

// hookRun runs the hook script on the router and returns its exit status and how long it took there, so the SSH
// round trip, which can be slow on the emulator, does not count.
func hookRun(t *testing.T) (rc int, elapsed time.Duration) {
	t.Helper()
	out := rt.MustRun(t, `a=$(cut -d' ' -f1 /proc/uptime); type=iptables table=mangle `+hookFile+`; rc=$?; b=$(cut -d' ' -f1 /proc/uptime); echo "$rc $a $b"`)
	var a, b float64
	if _, err := fmt.Sscanf(strings.TrimSpace(out), "%d %f %f", &rc, &a, &b); err != nil {
		t.Fatalf("unexpected hook output %q: %v", out, err)
	}
	return rc, time.Duration((b - a) * float64(time.Second))
}

// ndm runs the hook synchronously, so it must neither fail nor hang when the daemon is gone: with no socket at all,
// and with a socket file left behind that nobody listens on.
func TestHookScriptWithoutDaemonIsHarmless(t *testing.T) {
	StopAll(t)
	rt.Run("rm -f " + sockFile)
	t.Run("no socket", func(t *testing.T) {
		rc, took := hookRun(t)
		if rc != 0 || took > 2*time.Second {
			t.Errorf("hook: status %d after %s", rc, took)
		}
	})
	t.Run("stale socket file", func(t *testing.T) {
		// socat leaves the socket file behind when killed.
		rt.MustRun(t, "rm -f "+sockFile+"; (socat UNIX-LISTEN:"+sockFile+" /dev/null &) ; sleep 1; killall socat; test -S "+sockFile)
		t.Cleanup(func() { rt.Run("rm -f " + sockFile) })
		rc, took := hookRun(t)
		if rc != 0 || took > 2*time.Second {
			t.Errorf("hook: status %d after %s", rc, took)
		}
	})
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
	settleNDMAfter(t)
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
	settleNDMAfter(t)
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
	settleNDMAfter(t)
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
