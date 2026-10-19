package e2e

import (
	"encoding/json"
	"fmt"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/miekg/dns"
)

// ipsetName is the name MagiTrickle gives to the set of a group ("mt_<id>_4" / "_6").
func ipsetName(groupID string, v6 bool) string {
	if v6 {
		return "mt_" + groupID + "_6"
	}
	return "mt_" + groupID + "_4"
}

func ipsetExists(t testing.TB, name string) bool {
	t.Helper()
	_, err := rt.Run("ipset list -n " + shq(name) + " >/dev/null 2>&1 && ipset list -n | grep -qx " + shq(name))
	return err == nil
}

// ipsetMembers returns the members of a set without timeouts, or nil if the set does not exist.
func ipsetMembers(t testing.TB, name string) []string {
	t.Helper()
	out, err := rt.Run("ipset list " + shq(name))
	if err != nil {
		return nil
	}
	var members []string
	in := false
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimSpace(line)
		if line == "Members:" {
			in = true
			continue
		}
		if in && line != "" {
			members = append(members, strings.Fields(line)[0])
		}
	}
	return members
}

// ipsetTimeout returns the remaining timeout of a member in seconds, -1 if absent.
func ipsetTimeout(t testing.TB, name, member string) int {
	t.Helper()
	out, _ := rt.Run("ipset list " + shq(name))
	re := regexp.MustCompile(`(?m)^` + regexp.QuoteMeta(member) + ` timeout (\d+)`)
	m := re.FindStringSubmatch(out)
	if m == nil {
		return -1
	}
	var n int
	fmt.Sscan(m[1], &n)
	return n
}

// iptSave dumps one table; v6 selects ip6tables.
func iptSave(t testing.TB, table string, v6 bool) string {
	t.Helper()
	bin := "iptables-save"
	if v6 {
		bin = "ip6tables-save"
	}
	return rt.MustRun(t, fmt.Sprintf("%s -t %s", bin, table))
}

// mtRules returns only the lines of an iptables-save dump that concern MagiTrickle chains.
func mtRules(dump string) []string {
	var out []string
	for _, l := range strings.Split(dump, "\n") {
		if strings.Contains(l, "MT_") {
			out = append(out, l)
		}
	}
	return out
}

func containsLine(lines []string, sub string) bool {
	for _, l := range lines {
		if strings.Contains(l, sub) {
			return true
		}
	}
	return false
}

// exchange sends a query the way a LAN client does: from this machine to 127.0.0.1:8053, which the emulator
// forwards (UDP and TCP) from its virtual LAN host to 192.168.1.1:53. The packet crosses the router's nat
// PREROUTING, where MT_DNSOR redirects it to the MagiTrickle proxy.
func exchange(network, name, qtype string) (*dns.Msg, error) {
	qt, ok := dns.StringToType[strings.ToUpper(qtype)]
	if !ok {
		return nil, fmt.Errorf("unknown query type %q", qtype)
	}
	m := new(dns.Msg)
	if qt == dns.TypePTR {
		m.SetQuestion(dns.Fqdn(name), qt)
	} else {
		m.SetQuestion(dns.Fqdn(name), qt)
	}
	c := &dns.Client{Net: network, Timeout: 10 * time.Second}
	r, _, err := c.Exchange(m, "127.0.0.1:8053")
	return r, err
}

// answers renders an answer section like `dig +short`.
func answers(r *dns.Msg) []string {
	var out []string
	for _, rr := range r.Answer {
		switch v := rr.(type) {
		case *dns.A:
			out = append(out, v.A.String())
		case *dns.AAAA:
			out = append(out, v.AAAA.String())
		case *dns.CNAME:
			out = append(out, v.Target)
		}
	}
	return out
}

// query asks the MagiTrickle DNS proxy over UDP from the client side and returns the answers.
func query(t testing.TB, name, qtype string) []string {
	t.Helper()
	return queryNet(t, "udp", name, qtype)
}

func queryNet(t testing.TB, network, name, qtype string) []string {
	t.Helper()
	r, err := exchange(network, name, qtype)
	if err != nil {
		t.Fatalf("%s query %s %s: %v", network, name, qtype, err)
	}
	return answers(r)
}

// queryRcode returns the rcode name (NOERROR, NXDOMAIN, ...) of the response.
func queryRcode(t testing.TB, name, qtype string) string {
	t.Helper()
	r, err := exchange("udp", name, qtype)
	if err != nil {
		t.Fatalf("query %s %s: %v", name, qtype, err)
	}
	return dns.RcodeToString[r.Rcode]
}

// ZoneRecord mirrors helper.Record.
type ZoneRecord struct {
	Name  string `json:"name"`
	Type  string `json:"type"`
	Value string `json:"value,omitempty"`
	TTL   uint32 `json:"ttl,omitempty"`

	NoFollow bool `json:"nofollow,omitempty"`
}

// SetZone replaces the zone served by the scripted upstream DNS.
func SetZone(t testing.TB, recs ...ZoneRecord) {
	t.Helper()
	b, _ := json.Marshal(recs)
	rt.MustPut(t, "/opt/tmp/zone.json", b, 0o644)
}
