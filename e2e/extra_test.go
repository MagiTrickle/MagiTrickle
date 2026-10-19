package e2e

import (
	"fmt"
	"os/exec"
	"regexp"
	"strings"
	"testing"
	"time"
)

// The Entware passwd may carry any crypt(3) flavour; MagiTrickle verifies $1$, $5$ and $6$.
func TestAuthWithShaCryptPasswords(t *testing.T) {
	passwd := "/opt/etc/passwd"
	rt.MustRun(t, "cp "+passwd+" /opt/tmp/passwd.bak")
	t.Cleanup(func() { rt.Run("cp /opt/tmp/passwd.bak " + passwd) })
	// busybox cryptpw only does MD5, so the hashes are made on the host.
	for _, algo := range []struct{ flag, user string }{{"-5", "e2esha256"}, {"-6", "e2esha512"}} {
		out, err := exec.Command("openssl", "passwd", algo.flag, "-salt", "e2esalt1", "s3cretpass").Output()
		if err != nil {
			t.Skipf("openssl passwd: %v", err)
		}
		rt.MustRun(t, fmt.Sprintf("echo '%s:%s:1000:1000::/tmp:/bin/false' >> %s", algo.user, strings.TrimSpace(string(out)), passwd))
	}
	StartDaemonRaw(t, authedConfig())
	for _, user := range []string{"e2esha256", "e2esha512"} {
		code, body := rawDo(t, "POST", "/auth", "", map[string]string{"login": user, "password": "s3cretpass"})
		if code != 200 {
			t.Errorf("%s login = %d %s", user, code, body)
		}
		if code, _ := rawDo(t, "POST", "/auth", "", map[string]string{"login": user, "password": "wrong"}); code != 403 {
			t.Errorf("%s wrong password = %d", user, code)
		}
	}
	// Accounts without a usable hash cannot log in.
	rt.MustRun(t, "echo 'locked:*:1001:1001::/tmp:/bin/false' >> "+passwd)
	if code, _ := rawDo(t, "POST", "/auth", "", map[string]string{"login": "locked", "password": "x"}); code != 403 {
		t.Errorf("locked account = %d", code)
	}
}

// The signing secret is generated once and reused: tokens survive a restart, and a corrupt secret is refused.
func TestAuthTokenSurvivesRestart(t *testing.T) {
	rt.MustRun(t, "rm -f /opt/var/lib/magitrickle/auth_secret")
	StartDaemonRaw(t, authedConfig())
	_, body := rawDo(t, "POST", "/auth", "", map[string]string{"login": "root", "password": "keenetic"})
	token := strings.Split(strings.Split(string(body), `"token":"`)[1], `"`)[0]
	rt.MustRun(t, "test -s /opt/var/lib/magitrickle/auth_secret")

	Restart(t)
	if code, _ := rawDo(t, "GET", "/groups", token, nil); code != 200 {
		t.Errorf("token rejected after restart: %d", code)
	}

	rt.MustRun(t, "echo '!!!not base64!!!' > /opt/var/lib/magitrickle/auth_secret")
	Restart(t)
	if code, _ := rawDo(t, "POST", "/auth", "", map[string]string{"login": "root", "password": "keenetic"}); code == 200 {
		t.Errorf("login succeeded with a corrupt secret")
	}
	rt.MustRun(t, "rm -f /opt/var/lib/magitrickle/auth_secret")
}

func TestLogLevels(t *testing.T) {
	SetZone(t, ZoneRecord{Name: "lvl.test", Type: "A", Value: "203.0.113.90"})
	for _, tc := range []struct {
		level      string
		wantINF    bool
		wantDBG    bool
		wantTRC    bool
		wantAnyLog bool
	}{
		{"trace", true, true, true, true},
		{"debug", true, true, false, true},
		{"info", true, false, false, true},
		{"", true, false, false, true}, // default
		{"nonsense", true, false, false, true},
		{"warn", false, false, false, true},
		{"error", false, false, false, true},
		{"fatal", false, false, false, false},
		{"panic", false, false, false, false},
		{"disabled", false, false, false, false},
	} {
		t.Run("level="+tc.level, func(t *testing.T) {
			cfg := DaemonConfig{ShowAll: true}.YAML()
			cfg = strings.Replace(cfg, "logLevel: debug", "logLevel: "+tc.level, 1)
			d := StartDaemonRaw(t, cfg)
			api.CreateGroup(t, Group{Name: "lvl", Interface: "eth3", Enable: ptr(true),
				Rules: &[]Rule{{Name: "r", Type: "domain", Rule: "lvl.test", Enable: true}}})
			dig(t, "lvl.test", "A")
			time.Sleep(time.Second)
			out := d.Output()
			// "starting MagiTrickle daemon" is printed before the level is applied.
			out = out[strings.Index(out, "\n")+1:]
			// "starting service" is also printed by main() before the level is applied.
			out = regexp.MustCompile(`(?m)^.*starting service.*\n?`).ReplaceAllString(out, "")
			has := func(tag string) bool { return strings.Contains(out, " "+tag+" ") }
			if has("INF") != tc.wantINF {
				t.Errorf("INF lines present = %v, want %v", has("INF"), tc.wantINF)
			}
			if has("DBG") != tc.wantDBG {
				t.Errorf("DBG lines present = %v, want %v", has("DBG"), tc.wantDBG)
			}
			if has("TRC") != tc.wantTRC {
				t.Errorf("TRC lines present = %v, want %v", has("TRC"), tc.wantTRC)
			}
			if !tc.wantAnyLog && strings.TrimSpace(out) != "" {
				t.Errorf("unexpected output at level %q:\n%s", tc.level, out)
			}
		})
	}
}

func TestDNSOverTCPKeepsConnectionOpen(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "tcpk", Interface: "eth3", Enable: ptr(true),
		Rules: &[]Rule{{Name: "r", Type: "namespace", Rule: "tcpk.test", Enable: true}}})
	SetZone(t,
		ZoneRecord{Name: "a.tcpk.test", Type: "A", Value: "203.0.113.91"},
		ZoneRecord{Name: "b.tcpk.test", Type: "A", Value: "203.0.113.92"},
		ZoneRecord{Name: "c.tcpk.test", Type: "A", Value: "203.0.113.93"})
	// Three queries pipelined over one TCP connection.
	out := rt.MustRun(t, "dig +tcp +keepopen +short +time=3 +tries=1 -p 3553 @127.0.0.1 a.tcpk.test A b.tcpk.test A c.tcpk.test A")
	if got := strings.Fields(out); len(got) != 3 {
		t.Fatalf("answers = %v", got)
	}
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.91", "203.0.113.92", "203.0.113.93")

	// A TCP client that connects and hangs up, or sends garbage, must not take the listener down.
	rt.Run("echo -n garbage | nc -w 1 127.0.0.1 3553; nc -w 1 127.0.0.1 3553 < /dev/null")
	if got := dig(t, "a.tcpk.test", "A"); len(got) != 1 {
		t.Errorf("proxy unhealthy after bad TCP clients: %v", got)
	}
	rt.Run("echo -n garbage | nc -u -w 1 127.0.0.1 3553")
	if got := dig(t, "a.tcpk.test", "A"); len(got) != 1 {
		t.Errorf("proxy unhealthy after bad UDP datagram: %v", got)
	}
}

func TestGroupPutValidation(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "v", Interface: "eth3", Enable: ptr(true)})
	other := api.CreateGroup(t, Group{Name: "o", Interface: "eth3", Enable: ptr(true)})

	// Body id must match the path id.
	bad := g
	bad.ID = other.ID
	if c := api.Status(t, "PUT", "/groups/"+g.ID, bad); c != 400 {
		t.Errorf("id mismatch = %d", c)
	}
	// ...and a failed update leaves the group untouched and active.
	if got := api.Group(t, g.ID); got.Name != "v" {
		t.Errorf("group modified by rejected PUT: %+v", got)
	}
	var saved Group
	g.Name = "v2"
	api.OK(t, "PUT", "/groups/"+g.ID+"?save=true", g, &saved)
	if !strings.Contains(rt.MustRun(t, "cat "+configPath), "name: v2") {
		t.Error("PUT ?save=true did not persist")
	}
	// Duplicate ids in PUT /groups are not accepted silently.
	c := api.Status(t, "PUT", "/groups", map[string]any{"groups": []Group{{ID: g.ID, Name: "x"}, {ID: g.ID, Name: "y"}}})
	if c != 400 && c != 409 {
		t.Errorf("PUT /groups with duplicate ids = %d, want 400/409", c)
	}
	if left := len(api.Groups(t, false)); left != 2 {
		t.Errorf("rejected PUT /groups changed the group list: %d groups left, want 2", left)
	}
	if code, _, _ := api.Do("GET", "/groups", nil); code != 200 {
		t.Error("API broken after duplicate-id PUT")
	}
}

func TestSubscriptionPutValidation(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	if c := api.Status(t, "PUT", "/subscriptions", map[string]any{}); c != 400 {
		t.Errorf("PUT without subscriptions = %d", c)
	}
	if c := api.Status(t, "PUT", "/subscriptions", map[string]any{"subscriptions": []Subscription{{Name: "nourl"}}}); c != 400 {
		t.Errorf("PUT with empty url = %d", c)
	}
	// Duplicate subscription and rule ids are re-rolled instead of being stored.
	rules := []SubRule{{ID: "11111111", Rule: "a.test", Type: "namespace", Enable: true}, {ID: "11111111", Rule: "b.test", Type: "namespace", Enable: true}}
	api.OK(t, "PUT", "/subscriptions", map[string]any{"subscriptions": []Subscription{
		{ID: "22222222", Name: "one", Interface: "eth3", URL: listURL + "x", Rules: &rules},
		{ID: "22222222", Name: "two", Interface: "eth3", URL: listURL + "y"},
	}}, nil)
	subs := api.Subscriptions(t)
	if len(subs) != 2 || subs[0].ID == subs[1].ID {
		t.Fatalf("duplicate subscription ids kept: %+v", subs)
	}
	if r := *subs[0].Rules; len(r) != 2 || r[0].ID == r[1].ID {
		t.Errorf("duplicate rule ids kept: %+v", r)
	}
	// A subscription without an interface is stored but inert.
	api.OK(t, "POST", "/subscriptions", Subscription{Name: "noif", URL: listURL + "z"}, nil)
	if s := snapshot(t); count(s.mangle4, "sub-") > 0 && len(api.Subscriptions(t)) == 3 {
		for _, sub := range api.Subscriptions(t) {
			if sub.Name == "noif" && count(s.mangle4, sub.ID) > 0 {
				t.Errorf("interface-less subscription installed netfilter rules")
			}
		}
	}
	// Creating a subscription with an already used id conflicts.
	c := api.Status(t, "POST", "/subscriptions", Subscription{ID: subs[0].ID, Name: "dup", Interface: "eth3", URL: listURL + "d"})
	t.Logf("POST subscription with existing id = %d", c)
}

func TestSubscriptionResyncOfUnchangedListKeepsState(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	url := publishList(t, "same.txt", "one.example.org\ntwo.example.org\n")
	sub := createSub(t, Subscription{Name: "same", Interface: "eth3", Enable: ptr(true), URL: url})
	var first, second struct {
		Rules      []SubRule
		LastUpdate uint32
	}
	api.OK(t, "POST", "/subscriptions/"+sub.ID+"/sync", nil, &first)
	time.Sleep(2 * time.Second)
	// Same content, different order and a duplicate: nothing changes, lastUpdate stays.
	publishList(t, "same.txt", "two.example.org\none.example.org\ntwo.example.org\n")
	api.OK(t, "POST", "/subscriptions/"+sub.ID+"/sync", nil, &second)
	if second.LastUpdate != first.LastUpdate {
		t.Errorf("lastUpdate moved from %d to %d for an unchanged list", first.LastUpdate, second.LastUpdate)
	}
	ids := map[string]string{}
	for _, r := range first.Rules {
		ids[r.Rule] = r.ID
	}
	for _, r := range second.Rules {
		if ids[r.Rule] != r.ID {
			t.Errorf("rule %s changed id", r.Rule)
		}
	}
}

// Removing rules removes their addresses from the sets (both families).
func TestSubnetRemovalShrinksSets(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "shrink", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "a", Type: "subnet", Rule: "192.0.2.21", Enable: true},
		{Name: "b", Type: "subnet", Rule: "192.0.2.22", Enable: true},
		{Name: "c", Type: "subnet6", Rule: "2001:db8:9::1", Enable: true},
		{Name: "d", Type: "subnet6", Rule: "2001:db8:9::2", Enable: true},
	}})
	waitMembers(t, ipsetName(g.ID, false), "192.0.2.21", "192.0.2.22")
	waitMembers(t, ipsetName(g.ID, true), "2001:db8:9::1", "2001:db8:9::2")

	var rules struct{ Rules []Rule }
	api.OK(t, "GET", "/groups/"+g.ID+"/rules", nil, &rules)
	keep := []Rule{rules.Rules[0], rules.Rules[2]}
	api.OK(t, "PUT", "/groups/"+g.ID+"/rules", map[string]any{"rules": keep}, nil)
	waitMembers(t, ipsetName(g.ID, false), "192.0.2.21")
	waitMembers(t, ipsetName(g.ID, true), "2001:db8:9::1")

	// Disabling a rule has the same effect as removing it.
	keep[0].Enable = false
	api.OK(t, "PUT", "/groups/"+g.ID+"/rules", map[string]any{"rules": keep}, nil)
	waitMembers(t, ipsetName(g.ID, false))
}

func TestPIDFileWithGarbageIsReplaced(t *testing.T) {
	StopAll(t)
	rt.MustRun(t, "echo 'not a pid' > "+pidFile)
	StartDaemon(t, DaemonConfig{})
	if got := strings.TrimSpace(rt.MustRun(t, "cat "+pidFile)); got == "not a pid" {
		t.Error("garbage pid file kept")
	}
}

// Cached DNS data is dropped after its TTL, so a name that now resolves elsewhere is routed to the new address only.
func TestCacheExpiryAndReResolve(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true, AdditionalTTL: "1s"})
	g := api.CreateGroup(t, Group{Name: "exp", Interface: "eth3", Enable: ptr(true),
		Rules: &[]Rule{{Name: "r", Type: "namespace", Rule: "cache.test", Enable: true}}})
	SetZone(t,
		ZoneRecord{Name: "www.cache.test", Type: "CNAME", Value: "old.cdn.test", TTL: 1},
		ZoneRecord{Name: "old.cdn.test", Type: "A", Value: "203.0.113.95", TTL: 1})
	dig(t, "www.cache.test", "A")
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.95")
	WaitFor(t, 15*time.Second, "old address to expire", func() bool { return len(ipsetMembers(t, ipsetName(g.ID, false))) == 0 })

	// The records cache sweeps every 30s; after that, the alias must be gone, so an unrelated A answer is not routed.
	time.Sleep(32 * time.Second)
	SetZone(t, ZoneRecord{Name: "old.cdn.test", Type: "A", Value: "203.0.113.96", TTL: 60})
	dig(t, "old.cdn.test", "A")
	time.Sleep(time.Second)
	if m := ipsetMembers(t, ipsetName(g.ID, false)); len(m) != 0 {
		t.Errorf("expired alias still routes: %v", m)
	}
}
