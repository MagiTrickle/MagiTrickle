package e2e

import (
	"net"
	"regexp"
	"testing"
	"time"

	"github.com/miekg/dns"
)

// queryLAN asks the router's port 53 the way a LAN client does. The emulator forwards 127.0.0.1:8053 (UDP and
// TCP) from its virtual LAN host to 192.168.1.1:53, so the packets traverse nat PREROUTING and MT_DNSOR.
func queryLAN(t testing.TB, network, name string, qtype uint16) *dns.Msg {
	t.Helper()
	c := &dns.Client{Net: network, Timeout: 5 * time.Second}
	m := new(dns.Msg)
	m.SetQuestion(dns.Fqdn(name), qtype)
	r, _, err := c.Exchange(m, net.JoinHostPort("127.0.0.1", "8053"))
	if err != nil {
		t.Fatalf("%s query %s: %v", network, name, err)
	}
	return r
}

// A query to port 53 from a LAN client is redirected to MagiTrickle, which feeds the group's ipset.
func TestLANClientDNSIsRedirectedToProxy(t *testing.T) {
	for _, network := range []string{"tcp", "udp"} {
		t.Run(network, func(t *testing.T) {
			d := StartDaemon(t, DaemonConfig{ShowAll: true})
			name := "lan-" + network + ".test"
			g := api.CreateGroup(t, Group{Name: "lan", Interface: "eth3", Enable: ptr(true),
				Rules: &[]Rule{{Name: "r", Type: "domain", Rule: name, Enable: true}}})
			SetZone(t, ZoneRecord{Name: name, Type: "A", Value: "203.0.113.120"})
			WaitFor(t, 5*time.Second, "DNSOR", func() bool { return jumpPresent(t, "iptables", "nat", "PREROUTING", "MT_DNSOR") })

			r := queryLAN(t, network, name, dns.TypeA)
			if len(r.Answer) != 1 || r.Answer[0].(*dns.A).A.String() != "203.0.113.120" {
				t.Fatalf("answer: %v", r.Answer)
			}
			waitMembers(t, ipsetName(g.ID, false), "203.0.113.120")
			// The client address the proxy saw is the emulated LAN host, not loopback: the packet really
			// went through nat PREROUTING.
			d.WaitLog(`requested record.*clientAddr=192\.168\.1\.\d+:\d+.*name=`+regexp.QuoteMeta(name), 5*time.Second)
		})
	}
}

// Without the redirect the query goes to the system resolver and MagiTrickle never sees it.
func TestLANClientDNSBypassesProxyWithoutRemap(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true, DisableRemap53: true})
	SetZone(t, ZoneRecord{Name: "noremap.test", Type: "A", Value: "203.0.113.121"})
	c := &dns.Client{Net: "udp", Timeout: 5 * time.Second}
	m := new(dns.Msg)
	m.SetQuestion("noremap.test.", dns.TypeA)
	_, _, _ = c.Exchange(m, "127.0.0.1:8053") // answer comes from ndm (or times out); either way not from us
	time.Sleep(time.Second)
	if d.CountLog(`requested record.*noremap\.test`) != 0 {
		t.Error("the proxy saw a query although disableRemap53 is set")
	}
}

// Stopping the daemon removes the redirect, so the LAN keeps resolving through ndm.
func TestLANClientDNSWorksAfterDaemonStops(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{ShowAll: true})
	WaitFor(t, 5*time.Second, "DNSOR", func() bool { return jumpPresent(t, "iptables", "nat", "PREROUTING", "MT_DNSOR") })
	rci.StaticHost(t, "ndm-own.test", "203.0.113.122")
	if err := d.Stop(15 * time.Second); err != nil {
		t.Fatal(err)
	}
	r := queryLAN(t, "tcp", "ndm-own.test", dns.TypeA)
	if len(r.Answer) != 1 {
		t.Errorf("ndm did not answer after the daemon stopped: %v", r.Answer)
	}
}
