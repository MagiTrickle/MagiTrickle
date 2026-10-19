package e2e

import (
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/miekg/dns"
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
	got := query(t, "www.cn.dns.test", "A")
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
	query(t, "target.cdn.dns.test", "A")
	time.Sleep(500 * time.Millisecond)
	if m := ipsetMembers(t, ipsetName(g.ID, false)); len(m) != 0 {
		t.Fatalf("unrelated name was routed: %v", m)
	}
	query(t, "alias.dns.test", "A")
	waitMembers(t, ipsetName(g.ID, false), "203.0.113.51")
}

func TestIPSetTimeoutIsTTLPlusAdditional(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true, AdditionalTTL: "200s"})
	g := api.CreateGroup(t, Group{Name: "ttl", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "r", Type: "domain", Rule: "ttl.dns.test", Enable: true}}})
	SetZone(t, ZoneRecord{Name: "ttl.dns.test", Type: "A", Value: "203.0.113.52", TTL: 100})
	query(t, "ttl.dns.test", "A")
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
		if got := query(t, "v6.dns.test", "AAAA"); len(got) != 0 {
			t.Errorf("AAAA must be dropped, got %v", got)
		}
		query(t, "v6.dns.test", "A")
		waitMembers(t, ipsetName(g.ID, false), "203.0.113.53")
		t.Logf("v6 set after dropped AAAA: %v", ipsetMembers(t, ipsetName(g.ID, true)))
	})

	t.Run("AAAA kept when disableDropAAAA", func(t *testing.T) {
		StartDaemon(t, DaemonConfig{ShowAll: true, DisableAAAA: true})
		g := api.CreateGroup(t, Group{Name: "v6", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
			{Name: "r", Type: "domain", Rule: "v6.dns.test", Enable: true}}})
		SetZone(t, ZoneRecord{Name: "v6.dns.test", Type: "AAAA", Value: "2001:db8::53"})
		if got := query(t, "v6.dns.test", "AAAA"); len(got) != 1 || got[0] != "2001:db8::53" {
			t.Fatalf("AAAA = %v", got)
		}
		waitMembers(t, ipsetName(g.ID, true), "2001:db8::53")
	})
}

func TestFakePTR(t *testing.T) {
	t.Run("answers NXDOMAIN without asking upstream", func(t *testing.T) {
		d := StartDaemon(t, DaemonConfig{ShowAll: true})
		SetZone(t, ZoneRecord{Name: "9.113.0.203.in-addr.arpa", Type: "A", Value: "1.2.3.4"}) // would answer if asked
		if st := queryRcode(t, "9.113.0.203.in-addr.arpa", "PTR"); st != "NXDOMAIN" {
			t.Errorf("status = %s", st)
		}
		_ = d
	})
	t.Run("disabled: forwarded to upstream", func(t *testing.T) {
		StartDaemon(t, DaemonConfig{ShowAll: true, DisableFakePTR: true})
		SetZone(t)
		if st := queryRcode(t, "9.113.0.203.in-addr.arpa", "PTR"); st != "NOERROR" {
			t.Errorf("status = %s", st)
		}
	})
}

func TestNXDOMAINNotRouted(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "nx", Interface: "eth3", Enable: ptr(true), Rules: &[]Rule{
		{Name: "r", Type: "namespace", Rule: "nx.dns.test", Enable: true}}})
	SetZone(t, ZoneRecord{Name: "gone.nx.dns.test", Type: "NXDOMAIN"})
	if st := queryRcode(t, "gone.nx.dns.test", "A"); st != "NXDOMAIN" {
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
	if got := queryNet(t, "tcp", "tcp.dns.test", "A"); len(got) != 1 || got[0] != "203.0.113.54" {
		t.Fatalf("tcp answer = %v", got)
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
	var wg sync.WaitGroup
	sem := make(chan struct{}, 16)
	errs := make(chan error, n)
	for i := 1; i <= n; i++ {
		wg.Add(1)
		sem <- struct{}{}
		go func(i int) {
			defer wg.Done()
			defer func() { <-sem }()
			if _, err := exchange("udp", fmt.Sprintf("h%d.par.dns.test", i), "A"); err != nil {
				errs <- err
			}
		}(i)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Errorf("parallel query failed: %v", err)
	}
	waitMembers(t, ipsetName(g.ID, false), want...)
}

func TestUnreachableUpstream(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true, Upstream: "127.0.0.1:5999"})
	if r, err := exchange("udp", "down.dns.test", "A"); err == nil && r.Rcode == dns.RcodeSuccess {
		t.Errorf("unexpected NOERROR with dead upstream: %v", r)
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
	query(t, "log.dns.test", "A")
	d.WaitLog(`requested record.*name=log\.dns\.test\.`, 5*time.Second)
}
