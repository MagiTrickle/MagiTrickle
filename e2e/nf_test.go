package e2e

import (
	"encoding/json"
	"fmt"
	"regexp"
	"strings"
	"testing"
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

// dig asks the MagiTrickle DNS proxy and returns the short answers.
func dig(t testing.TB, name, qtype string) []string {
	t.Helper()
	out, err := rt.Run(fmt.Sprintf("dig +short +time=3 +tries=1 -p 3553 @127.0.0.1 %s %s", shq(name), qtype))
	if err != nil {
		t.Fatalf("dig %s %s: %v\n%s", name, qtype, err, out)
	}
	var res []string
	for _, l := range strings.Split(strings.TrimSpace(out), "\n") {
		if l != "" {
			res = append(res, l)
		}
	}
	return res
}

// digStatus returns the rcode string from a full dig output (NOERROR, NXDOMAIN, ...).
func digStatus(t testing.TB, name, qtype string) string {
	t.Helper()
	out, _ := rt.Run(fmt.Sprintf("dig +time=3 +tries=1 -p 3553 @127.0.0.1 %s %s", shq(name), qtype))
	m := regexp.MustCompile(`status: (\w+)`).FindStringSubmatch(out)
	if m == nil {
		t.Fatalf("no status in dig output:\n%s", out)
	}
	return m[1]
}

// ZoneRecord mirrors helper.Record.
type ZoneRecord struct {
	Name  string `json:"name"`
	Type  string `json:"type"`
	Value string `json:"value,omitempty"`
	TTL   uint32 `json:"ttl,omitempty"`
}

// SetZone replaces the zone served by the scripted upstream DNS.
func SetZone(t testing.TB, recs ...ZoneRecord) {
	t.Helper()
	b, _ := json.Marshal(recs)
	rt.MustPut(t, "/opt/tmp/zone.json", b, 0o644)
}
