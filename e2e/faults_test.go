package e2e

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

const fakeBin = "/opt/tmp/fakebin"
const failFlag = "/opt/tmp/ipt-fail"

// installFakeIPTables puts wrappers for iptables(6)-save/restore first in PATH. They behave like the real ones
// until the flag file exists, then fail like a kernel that refuses the ruleset.
func installFakeIPTables(t *testing.T) {
	t.Helper()
	rt.MustRun(t, "rm -f "+failFlag+"; rm -rf "+fakeBin+"; mkdir -p "+fakeBin)
	for _, name := range []string{"iptables-save", "iptables-restore", "ip6tables-save", "ip6tables-restore"} {
		real := strings.TrimSpace(rt.MustRun(t, "which "+name))
		script := fmt.Sprintf("#!/bin/sh\n[ -e %s ] && { echo 'simulated failure' >&2; exit 1; }\nexec %s \"$@\"\n", failFlag, real)
		rt.MustPut(t, fakeBin+"/"+name, []byte(script), 0o755)
	}
	t.Cleanup(func() { rt.Run("rm -f " + failFlag) })
}

func failIPTables(t testing.TB, on bool) {
	t.Helper()
	if on {
		rt.MustRun(t, "touch "+failFlag)
	} else {
		rt.MustRun(t, "rm -f "+failFlag)
	}
}

func startWithFakeIPTables(t *testing.T, cfg DaemonConfig) *Daemon {
	t.Helper()
	StopAll(t)
	rt.MustPut(t, configPath, []byte(cfg.YAML()), 0o644)
	return spawnDaemonWith(t, "PATH="+fakeBin+":/opt/sbin:/opt/bin:/sbin:/bin:/usr/sbin:/usr/bin")
}

func TestStartupFailsWhenIPTablesIsBroken(t *testing.T) {
	installFakeIPTables(t)
	StopAll(t)
	failIPTables(t, true)
	rt.MustPut(t, configPath, []byte(DaemonConfig{}.YAML()), 0o644)
	p, err := rt.Spawn("magitrickled-broken-ipt", "env PATH="+fakeBin+":$PATH "+daemonBin)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { p.Stop(5 * time.Second); StopAll(t) })
	WaitFor(t, 20*time.Second, "daemon to give up", p.Exited)
	if out := stripANSI(p.Log.String()); !strings.Contains(out, "failed to") {
		t.Errorf("no explanation in the output:\n%s", out)
	}
	if p.exitErr == nil {
		t.Error("daemon exited with status 0 although it failed to start")
	}
	if running(t) {
		t.Error("daemon keeps running with unusable iptables")
	}
	rt.Run("rm -f " + pidFile + " " + sockFile)
}

func TestIPTablesFailureAtRuntimeIsReportedAndRecoverable(t *testing.T) {
	installFakeIPTables(t)
	d := startWithFakeIPTables(t, DaemonConfig{ShowAll: true})

	failIPTables(t, true)
	code, body, err := api.Do("POST", "/groups", Group{Name: "doomed", Interface: "eth3", Enable: ptr(true)})
	if err != nil {
		t.Fatal(err)
	}
	if code == 200 {
		t.Fatalf("group created although iptables refuses rules: %s", body)
	}
	t.Logf("POST /groups with broken iptables = %d %s", code, strings.TrimSpace(string(body)))
	if d.Exited() {
		t.Fatal("daemon died on an iptables error")
	}

	// The netfilter.d hook only logs: ndm must not see a failing script because of us.
	if c := api.Status(t, "POST", "/system/hooks/netfilterd", map[string]string{"type": "iptables", "table": "nat"}); c != 200 {
		t.Errorf("hook with broken iptables = %d", c)
	}
	d.WaitLog(`error fixing iptables`, 5*time.Second)

	// Once iptables works again the daemon is usable without a restart.
	failIPTables(t, false)
	g := api.CreateGroup(t, Group{Name: "recovered", Interface: "eth3", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "chains after recovery", func() bool { return jumps(snapshot(t).mangle4, "PREROUTING", "MT_"+g.ID) == 1 })
}

func TestDisableFailsGracefullyWhenIPTablesIsBroken(t *testing.T) {
	installFakeIPTables(t)
	d := startWithFakeIPTables(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "g", Interface: "eth3", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "chains", func() bool { return jumps(snapshot(t).mangle4, "PREROUTING", "MT_"+g.ID) == 1 })

	failIPTables(t, true)
	for name, call := range map[string]func() int{
		"PUT group":    func() int { g.Name = "x"; return api.Status(t, "PUT", "/groups/"+g.ID, g) },
		"DELETE group": func() int { return api.Status(t, "DELETE", "/groups/"+g.ID, nil) },
		"PUT groups": func() int {
			return api.Status(t, "PUT", "/groups", map[string]any{"groups": []Group{{Name: "n", Interface: "eth3"}}})
		},
	} {
		t.Logf("%s with broken iptables = %d", name, call())
	}
	if d.Exited() {
		t.Fatal("daemon died")
	}
	failIPTables(t, false)
	if code, _, err := api.Do("GET", "/groups", nil); err != nil || code != 200 {
		t.Errorf("API unavailable afterwards: %v %d", err, code)
	}
}

// The API keeps working when the config cannot be persisted; only the explicit save reports it.
func TestConfigWriteFailures(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true})
	t.Cleanup(func() {
		rt.Run("rm -rf " + configPath + "; test -f /opt/tmp/state.bak || true")
	})

	t.Run("config file is a directory", func(t *testing.T) {
		rt.MustRun(t, "rm -f "+configPath+" && mkdir "+configPath)
		t.Cleanup(func() { rt.Run("rmdir " + configPath) })
		if c := api.Status(t, "POST", "/system/config/save", nil); c != 500 {
			t.Errorf("save into a directory = %d, want 500", c)
		}
		// ?save=true on a mutating call: the change is applied and the error is only logged.
		var g Group
		api.OK(t, "POST", "/groups?save=true", Group{Name: "unsaved", Interface: "eth3"}, &g)
		api.OK(t, "PUT", "/groups/"+g.ID+"?save=true", g, nil)
		api.OK(t, "POST", "/groups/"+g.ID+"/rules?save=true", Rule{Name: "r", Type: "domain", Rule: "x.test", Enable: true}, nil)
		api.OK(t, "DELETE", "/groups/"+g.ID+"?save=true", nil, nil)
		d.WaitLog(`failed to save config file`, 5*time.Second)
		if d.CountLog(`failed to save config file`) < 3 {
			t.Errorf("save failures logged %d times", d.CountLog(`failed to save config file`))
		}
	})

	t.Run("state directory is a file", func(t *testing.T) {
		dir := "/opt/var/lib/magitrickle"
		rt.MustRun(t, "rm -rf /opt/tmp/state.bak; mv "+dir+" /opt/tmp/state.bak && echo x > "+dir)
		t.Cleanup(func() { rt.Run("rm -rf " + dir + "; mv /opt/tmp/state.bak " + dir) })
		if c := api.Status(t, "POST", "/system/config/save", nil); c != 500 {
			t.Errorf("save with unusable state dir = %d, want 500", c)
		}
	})
}

// Values written in the pre-1.0 units (plain numbers) keep their meaning.
func TestLegacyConfigUnits(t *testing.T) {
	cfg := DaemonConfig{ShowAll: true}.YAML()
	cfg = strings.Replace(cfg, "additionalTTL: 1h", "additionalTTL: 3600", 1)
	cfg = strings.Replace(cfg, "timeout: 5s", "timeout: 2000", 1)
	StartDaemonRaw(t, cfg)
	g := api.CreateGroup(t, Group{Name: "legacy", Interface: "eth3", Enable: ptr(true),
		Rules: &[]Rule{{Name: "r", Type: "domain", Rule: "legacy.test", Enable: true}}})
	SetZone(t, ZoneRecord{Name: "legacy.test", Type: "A", Value: "203.0.113.97", TTL: 100})
	query(t, "legacy.test", "A")
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.97")
	if got := ipsetTimeout(t, ipsetName(g.ID, false), "203.0.113.97"); got < 3600 || got > 3700 {
		t.Errorf("timeout = %ds, want ttl 100 + 3600s (additionalTTL: 3600 means seconds)", got)
	}
}

// Every documented option must be honoured, not just parsed.
func TestConfigOptionsTakeEffect(t *testing.T) {
	cfg := DaemonConfig{ShowAll: true}.YAML()
	for from, to := range map[string]string{
		"chainPrefix: MT_":                "chainPrefix: XT_",
		"tablePrefix: mt_":                "tablePrefix: xs_",
		"startMarkTableIndex: 1298229097": "startMarkTableIndex: 5000",
		"port: 3553":                      "port: 4553",
	} {
		if !strings.Contains(cfg, from) {
			t.Fatalf("template has no %q", from)
		}
		cfg = strings.Replace(cfg, from, to, 1)
	}
	StartDaemonRaw(t, cfg)
	g := api.CreateGroup(t, Group{Name: "opts", Interface: "eth3", Enable: ptr(true),
		Rules: &[]Rule{{Name: "r", Type: "domain", Rule: "opts.test", Enable: true}}})
	SetZone(t, ZoneRecord{Name: "opts.test", Type: "A", Value: "203.0.113.98"})
	WaitFor(t, 5*time.Second, "custom chain prefix", func() bool {
		return strings.Contains(rt.MustRun(t, "iptables-save -t mangle"), "-A PREROUTING -j XT_"+g.ID)
	})
	out := rt.MustRun(t, "iptables-save -t nat")
	if !strings.Contains(out, "XT_DNSOR") || !strings.Contains(out, "--to-destination :4553") {
		t.Errorf("DNS redirect ignores chainPrefix/port:\n%s", out)
	}
	if strings.Contains(rt.MustRun(t, "iptables-save | grep -c '^-A.* MT_' || true"), "1") {
		t.Error("default MT_ prefix still in use")
	}
	// DNS listener moved to the configured port, and the set uses the configured prefix and mark.
	if res := strings.TrimSpace(rt.MustRun(t, "dig +short +time=3 +tries=1 -p 4553 @127.0.0.1 opts.test A")); res != "203.0.113.98" {
		t.Errorf("dig on the configured port = %q", res)
	}
	waitMembers(t, "xs_"+g.ID+"_4", "203.0.113.98")
	if rules := rt.MustRun(t, "ip rule | grep fwmark"); !strings.Contains(rules, "fwmark 0x1388 lookup 5000") {
		t.Errorf("startMarkTableIndex ignored: %q", rules)
	}
}

func TestHTTPCanBeDisabledAndMoved(t *testing.T) {
	t.Run("disabled", func(t *testing.T) {
		cfg := strings.Replace(DaemonConfig{ShowAll: true}.YAML(), "  httpWeb:\n    enabled: true", "  httpWeb:\n    enabled: false", 1)
		StopAll(t)
		rt.MustPut(t, configPath, []byte(cfg), 0o644)
		d := spawnDaemonNoWait(t)
		d.WaitLog(`HTTP WebUI disabled`, 10*time.Second)
		// The UNIX socket (used by the ndm hook) stays up.
		WaitFor(t, 10*time.Second, "unix socket", func() bool { _, err := rt.Run("test -S " + sockFile); return err == nil })
		if c, _, err := api.Do("GET", "/groups", nil); err == nil && c == 200 {
			t.Error("HTTP API answers although httpWeb.enabled is false")
		}
	})
	t.Run("moved", func(t *testing.T) {
		cfg := strings.Replace(DaemonConfig{ShowAll: true}.YAML(), "      port: 8080", "      port: 8090", 1)
		StopAll(t)
		rt.MustPut(t, configPath, []byte(cfg), 0o644)
		spawnDaemonNoWait(t)
		WaitFor(t, 10*time.Second, "API on 8090", func() bool {
			_, err := rt.Run("wget -qO- http://127.0.0.1:8090/api/v1/groups")
			return err == nil
		})
	})
}

func spawnDaemonNoWait(t *testing.T) *Daemon {
	t.Helper()
	p, err := rt.Spawn("magitrickled", daemonBin)
	if err != nil {
		t.Fatal(err)
	}
	d := &Daemon{Process: p, t: t}
	t.Cleanup(func() {
		if !p.Exited() {
			_ = p.Stop(15 * time.Second)
		}
		if t.Failed() {
			t.Logf("---- magitrickled output ----\n%s", stripANSI(p.Log.String()))
		}
	})
	return d
}

func TestUnknownLinkInConfig(t *testing.T) {
	StopAll(t)
	rt.MustPut(t, configPath, []byte(DaemonConfig{Links: []string{"doesnotexist0"}}.YAML()), 0o644)
	p, err := rt.Spawn("magitrickled-nolink", daemonBin)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { p.Stop(5 * time.Second); StopAll(t) })
	WaitFor(t, 15*time.Second, "daemon to exit", p.Exited)
	if out := stripANSI(p.Log.String()); !strings.Contains(out, "failed to find link doesnotexist0") {
		t.Errorf("output:\n%s", out)
	}
	rt.Run("rm -f " + pidFile + " " + sockFile)
}

// The startup cleanup removes everything that starts with the chain/set prefix, so a prefix that is empty or very
// short must stop the daemon before it touches anything.
func TestShortPrefixesRefuseToStart(t *testing.T) {
	rt.MustRun(t, "iptables -N Mforeign; ipset create mforeign hash:ip")
	t.Cleanup(func() { rt.Run("iptables -X Mforeign; ipset destroy mforeign") })

	for name, c := range map[string]struct{ from, to, want string }{
		"empty chain prefix":     {"chainPrefix: MT_", `chainPrefix: ""`, "chain prefix"},
		"one letter chain":       {"chainPrefix: MT_", "chainPrefix: M", "chain prefix"},
		"empty ipset prefix":     {"tablePrefix: mt_", `tablePrefix: ""`, "ipset prefix"},
		"two letters ipset name": {"tablePrefix: mt_", "tablePrefix: mt", "ipset prefix"},
	} {
		t.Run(name, func(t *testing.T) {
			StopAll(t)
			cfg := DaemonConfig{}.YAML()
			if !strings.Contains(cfg, c.from) {
				t.Fatalf("template has no %q", c.from)
			}
			rt.MustPut(t, configPath, []byte(strings.Replace(cfg, c.from, c.to, 1)), 0o644)
			p, err := rt.Spawn("magitrickled-short-prefix", daemonBin)
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { p.Stop(5 * time.Second); StopAll(t); rt.Run("rm -f " + pidFile + " " + sockFile) })
			WaitFor(t, 10*time.Second, "daemon to give up", p.Exited)
			if p.exitErr == nil {
				t.Error("daemon exited with status 0")
			}
			if out := stripANSI(p.Log.String()); !strings.Contains(out, c.want) || !strings.Contains(out, "too short") {
				t.Errorf("no explanation in the output:\n%s", out)
			}
			// Nothing was cleaned up, and nothing of ours was created.
			if !strings.Contains(iptSave(t, "filter", false), "Mforeign") {
				t.Error("foreign chain was deleted")
			}
			if !ipsetExists(t, "mforeign") {
				t.Error("foreign ipset was destroyed")
			}
		})
	}
}
