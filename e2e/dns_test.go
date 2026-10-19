package e2e

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

func TestCNAMEChain(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "cname", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "r", Type: "namespace", Rule: "cn.dns.test", Enable: true}}})
	SetZone(t,
		ZoneRecord{Name: "www.cn.dns.test", Type: "CNAME", Value: "mid.cdn.dns.test"},
		ZoneRecord{Name: "mid.cdn.dns.test", Type: "CNAME", Value: "edge.cdn.dns.test"},
		ZoneRecord{Name: "edge.cdn.dns.test", Type: "A", Value: "203.0.113.50"},
	)
	got := dig(t, "www.cn.dns.test", "A")
	if len(got) != 3 || got[len(got)-1] != "203.0.113.50" {
		t.Fatalf("dig = %v", got)
	}
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.50")
}

// A CNAME that arrives after its target is already cached must pull the cached address into the set.
func TestCNAMEForCachedTarget(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "cached", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "r", Type: "domain", Rule: "alias.dns.test", Enable: true}}})
	SetZone(t,
		ZoneRecord{Name: "target.cdn.dns.test", Type: "A", Value: "203.0.113.51", TTL: 600},
		ZoneRecord{Name: "alias.dns.test", Type: "CNAME", Value: "target.cdn.dns.test", NoFollow: true},
	)
	dig(t, "target.cdn.dns.test", "A")
	time.Sleep(500 * time.Millisecond)
	if m := ipsetMembers(t, ipsetName(g.ID, false)); len(m) != 0 {
		t.Fatalf("unrelated name was routed: %v", m)
	}
	dig(t, "alias.dns.test", "A")
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.51")
}

func TestIPSetTimeoutIsTTLPlusAdditional(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true, AdditionalTTL: "200s"})
	g := api.CreateGroup(t, Group{Name: "ttl", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "r", Type: "domain", Rule: "ttl.dns.test", Enable: true}}})
	SetZone(t, ZoneRecord{Name: "ttl.dns.test", Type: "A", Value: "203.0.113.52", TTL: 100})
	dig(t, "ttl.dns.test", "A")
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.52")
	if got := ipsetTimeout(t, ipsetName(g.ID, false), "203.0.113.52"); got < 250 || got > 300 {
		t.Errorf("timeout = %ds, want 250..300 (ttl 100 + additional 200)", got)
	}
}

func TestIPv6AndDropAAAA(t *testing.T) {
	t.Run("AAAA dropped from response by default", func(t *testing.T) {
		StartDaemon(t, DaemonConfig{ShowAll: true})
		g := api.CreateGroup(t, Group{Name: "v6", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
			{Name: "r", Type: "domain", Rule: "v6.dns.test", Enable: true}}})
		SetZone(t,
			ZoneRecord{Name: "v6.dns.test", Type: "A", Value: "203.0.113.53"},
			ZoneRecord{Name: "v6.dns.test", Type: "AAAA", Value: "2001:db8::53"})
		if got := dig(t, "v6.dns.test", "AAAA"); len(got) != 0 {
			t.Errorf("AAAA must be dropped, got %v", got)
		}
		dig(t, "v6.dns.test", "A")
		waitMembers(t, ipsetName(g.ID, false), "203.0.113.53")
		t.Logf("v6 set after dropped AAAA: %v", ipsetMembers(t, ipsetName(g.ID, true)))
	})

	t.Run("AAAA kept when disableDropAAAA", func(t *testing.T) {
		StartDaemon(t, DaemonConfig{ShowAll: true, DisableAAAA: true})
		g := api.CreateGroup(t, Group{Name: "v6", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
			{Name: "r", Type: "domain", Rule: "v6.dns.test", Enable: true}}})
		SetZone(t, ZoneRecord{Name: "v6.dns.test", Type: "AAAA", Value: "2001:db8::53"})
		if got := dig(t, "v6.dns.test", "AAAA"); len(got) != 1 || got[0] != "2001:db8::53" {
			t.Fatalf("AAAA = %v", got)
		}
		waitMembers(t, ipsetName(g.ID, true), "2001:db8::53")
	})
}

func TestFakePTR(t *testing.T) {
	t.Run("answers NXDOMAIN without asking upstream", func(t *testing.T) {
		d := StartDaemon(t, DaemonConfig{ShowAll: true})
		SetZone(t, ZoneRecord{Name: "9.113.0.203.in-addr.arpa", Type: "A", Value: "1.2.3.4"}) // would answer if asked
		if st := digStatus(t, "9.113.0.203.in-addr.arpa", "PTR"); st != "NXDOMAIN" {
			t.Errorf("status = %s", st)
		}
		_ = d
	})
	t.Run("disabled: forwarded to upstream", func(t *testing.T) {
		StartDaemon(t, DaemonConfig{ShowAll: true, DisableFakePTR: true})
		SetZone(t)
		if st := digStatus(t, "9.113.0.203.in-addr.arpa", "PTR"); st != "NOERROR" {
			t.Errorf("status = %s", st)
		}
	})
}

func TestNXDOMAINNotRouted(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "nx", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "r", Type: "namespace", Rule: "nx.dns.test", Enable: true}}})
	SetZone(t, ZoneRecord{Name: "gone.nx.dns.test", Type: "NXDOMAIN"})
	if st := digStatus(t, "gone.nx.dns.test", "A"); st != "NXDOMAIN" {
		t.Fatalf("status = %s", st)
	}
	d.WaitLog("unprocessable response", 5*time.Second)
	if m := ipsetMembers(t, ipsetName(g.ID, false)); len(m) != 0 {
		t.Errorf("set is not empty: %v", m)
	}
}

func TestDNSOverTCP(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "tcp", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "r", Type: "domain", Rule: "tcp.dns.test", Enable: true}}})
	SetZone(t, ZoneRecord{Name: "tcp.dns.test", Type: "A", Value: "203.0.113.54"})
	out := rt.MustRun(t, "dig +short +tcp +time=3 +tries=1 -p 3553 @127.0.0.1 tcp.dns.test A")
	if strings.TrimSpace(out) != "203.0.113.54" {
		t.Fatalf("dig +tcp = %q", out)
	}
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.54")
}

func TestDNSOnIPv6Listener(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	SetZone(t, ZoneRecord{Name: "six.dns.test", Type: "A", Value: "203.0.113.55"})
	out := rt.MustRun(t, "dig +short +time=3 +tries=1 -p 3553 @::1 six.dns.test A")
	if strings.TrimSpace(out) != "203.0.113.55" {
		t.Fatalf("dig @::1 = %q", out)
	}
}

func TestManyParallelQueries(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "par", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "r", Type: "namespace", Rule: "par.dns.test", Enable: true}}})
	const n = 60
	var zone []ZoneRecord
	var want []string
	for i := 1; i <= n; i++ {
		ip := fmt.Sprintf("203.0.114.%d", i)
		zone = append(zone, ZoneRecord{Name: fmt.Sprintf("h%d.par.dns.test", i), Type: "A", Value: ip})
		want = append(want, ip)
	}
	SetZone(t, zone...)
	rt.MustRun(t, fmt.Sprintf(`seq 1 %d | xargs -P 16 -I{} dig +short +time=3 +tries=1 -p 3553 @127.0.0.1 h{}.par.dns.test A >/dev/null`, n))
	waitMembers(t, ipsetName(g.ID, false), want...)
}

func TestUnreachableUpstream(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true, Upstream: "127.0.0.1:5999"})
	out, _ := rt.Run("dig +time=2 +tries=1 -p 3553 @127.0.0.1 down.dns.test A")
	if strings.Contains(out, "status: NOERROR") {
		t.Errorf("unexpected NOERROR with dead upstream:\n%s", out)
	}
	if d.Exited() {
		t.Fatal("daemon died")
	}
	if code, _, err := api.Do("GET", "/groups", nil); err != nil || code != 200 {
		t.Errorf("API unavailable after upstream failure: %v %d", err, code)
	}
}

func TestRequestLogging(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true})
	SetZone(t, ZoneRecord{Name: "log.dns.test", Type: "A", Value: "203.0.113.56"})
	dig(t, "log.dns.test", "A")
	d.WaitLog(`requested record.*name=log\.dns\.test\.`, 5*time.Second)
}
