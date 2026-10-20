package e2e

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

const listURL = "http://127.0.0.1:" + subscribePort + "/"

func publishList(t testing.TB, name, content string) string {
	t.Helper()
	rt.MustPut(t, "/opt/tmp/www/"+name, []byte(content), 0o644)
	return listURL + name
}

func subSet(id string, v6 bool) string {
	if v6 {
		return "mt_sub-" + id + "_6"
	}
	return "mt_sub-" + id + "_4"
}

func createSub(t testing.TB, s Subscription) Subscription {
	t.Helper()
	api.OK(t, "POST", "/subscriptions", s, nil)
	subs := api.Subscriptions(t)
	if len(subs) == 0 {
		t.Fatal("subscription not listed after create")
	}
	return subs[len(subs)-1]
}

func TestSubscriptionSyncParsesAndRoutes(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	url := publishList(t, "list1.txt", strings.Join([]string{
		"# a comment",
		"sub.example.org",
		"*.wild.example.org",
		"203.0.113.0/24",
		"2001:db8:5::/48",
		"",
		"sub.example.org", // duplicate
		"other.example.org, comma.example.org",
	}, "\n"))

	sub := createSub(t, Subscription{Name: "list", Interface: "eth3", Enable: ptr(true), URL: url})
	if len(sub.ID) != 8 || sub.URL != url {
		t.Fatalf("created: %+v", sub)
	}

	var res struct {
		Rules      []SubRule
		LastUpdate uint32
		URL        string
	}
	api.OK(t, "POST", "/subscriptions/"+sub.ID+"/sync", nil, &res)
	types := map[string]string{}
	for _, r := range res.Rules {
		types[r.Rule] = r.Type
		if !r.Enable {
			t.Errorf("rule %s is disabled after sync", r.Rule)
		}
	}
	want := map[string]string{
		"sub.example.org":    "namespace",
		"*.wild.example.org": "wildcard",
		"203.0.113.0/24":     "subnet",
		"2001:db8:5::/48":    "subnet6",
		"other.example.org":  "namespace",
		"comma.example.org":  "namespace",
	}
	if fmt.Sprint(types) != fmt.Sprint(want) {
		t.Errorf("parsed rules:\n got %v\nwant %v", types, want)
	}
	if res.LastUpdate == 0 || res.URL != url {
		t.Errorf("sync response: %+v", res)
	}

	// Subnet rules land in the subscription's sets right away; name rules on DNS answers.
	waitMembers(t, subSet(sub.ID, false), "203.0.113.0/24")
	waitMembers(t, subSet(sub.ID, true), "2001:db8:5::/48")
	SetZone(t,
		ZoneRecord{Name: "x.sub.example.org", Type: "A", Value: "198.51.100.77"},
		ZoneRecord{Name: "a.wild.example.org", Type: "A", Value: "198.51.100.78"},
		ZoneRecord{Name: "no.example.org", Type: "A", Value: "198.51.100.79"})
	query(t, "x.sub.example.org", "A")
	query(t, "a.wild.example.org", "A")
	query(t, "no.example.org", "A")
	waitMembers(t, subSet(sub.ID, false), "203.0.113.0/24", "198.51.100.77", "198.51.100.78")

	// Netfilter plumbing exists for the subscription as for a user group.
	s := snapshot(t)
	if !containsLine(s.mangle4, "-A PREROUTING -j MT_sub-"+sub.ID) {
		t.Errorf("no mangle jump for subscription:\n%s", s)
	}
}

func TestSubscriptionResyncKeepsRuleIDsAndDisabledState(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	url := publishList(t, "list2.txt", "keep.example.org\ndrop.example.org\n")
	sub := createSub(t, Subscription{Name: "resync", Interface: "eth3", Enable: ptr(true), URL: url})

	var first struct{ Rules []SubRule }
	api.OK(t, "POST", "/subscriptions/"+sub.ID+"/sync", nil, &first)
	if len(first.Rules) != 2 {
		t.Fatalf("rules: %+v", first.Rules)
	}

	// Disable one rule through PUT /subscriptions, then change the list upstream.
	cur := api.Subscriptions(t)[0]
	for i := range *cur.Rules {
		if (*cur.Rules)[i].Rule == "drop.example.org" {
			(*cur.Rules)[i].Enable = false
		}
	}
	api.OK(t, "PUT", "/subscriptions", map[string]any{"subscriptions": []Subscription{cur}}, nil)
	publishList(t, "list2.txt", "keep.example.org\ndrop.example.org\nnew.example.org\n")

	var second struct{ Rules []SubRule }
	api.OK(t, "POST", "/subscriptions/"+sub.ID+"/sync", nil, &second)
	byRule := map[string]SubRule{}
	for _, r := range second.Rules {
		byRule[r.Rule] = r
	}
	if len(byRule) != 3 {
		t.Fatalf("rules: %+v", second.Rules)
	}
	for _, f := range first.Rules {
		if byRule[f.Rule].ID != f.ID {
			t.Errorf("rule %s changed id %s -> %s", f.Rule, f.ID, byRule[f.Rule].ID)
		}
	}
	if byRule["drop.example.org"].Enable {
		t.Errorf("user-disabled rule re-enabled by sync")
	}
	if !byRule["new.example.org"].Enable {
		t.Errorf("new rule is disabled")
	}
}

func TestSubscriptionSyncErrors(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	if c := api.Status(t, "POST", "/subscriptions/00000001/sync", nil); c != 404 {
		t.Errorf("unknown subscription = %d", c)
	}
	if c := api.Status(t, "POST", "/subscriptions/zz/sync", nil); c != 400 {
		t.Errorf("bad id = %d", c)
	}
	sub := createSub(t, Subscription{Name: "dead", Interface: "eth3", Enable: ptr(true), URL: listURL + "missing.txt"})
	if c := api.Status(t, "POST", "/subscriptions/"+sub.ID+"/sync", nil); c != 502 {
		t.Errorf("404 upstream = %d, want 502", c)
	}
	sub2 := createSub(t, Subscription{Name: "unreachable", Interface: "eth3", Enable: ptr(true), URL: "http://127.0.0.1:1/x"})
	if c := api.Status(t, "POST", "/subscriptions/"+sub2.ID+"/sync", nil); c != 502 {
		t.Errorf("connection refused = %d, want 502", c)
	}
	if c := api.Status(t, "POST", "/subscriptions", Subscription{Name: "nourl", Interface: "eth3"}); c != 400 {
		t.Errorf("subscription without url = %d", c)
	}
	// Failed syncs must not wipe previously fetched rules.
	url := publishList(t, "list3.txt", "stay.example.org\n")
	sub3 := createSub(t, Subscription{Name: "stay", Interface: "eth3", Enable: ptr(true), URL: url})
	api.OK(t, "POST", "/subscriptions/"+sub3.ID+"/sync", nil, nil)
	rt.MustRun(t, "rm /opt/tmp/www/list3.txt")
	if c := api.Status(t, "POST", "/subscriptions/"+sub3.ID+"/sync", nil); c != 502 {
		t.Errorf("sync of vanished list = %d", c)
	}
	for _, s := range api.Subscriptions(t) {
		if s.ID == sub3.ID && (s.Rules == nil || len(*s.Rules) != 1) {
			t.Errorf("rules lost after failed sync: %+v", s)
		}
	}
}

func TestSubscriptionSyncWithURLOverride(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	a := publishList(t, "a.txt", "a.example.org\n")
	b := publishList(t, "b.txt", "b.example.org\n")
	sub := createSub(t, Subscription{Name: "override", Interface: "eth3", Enable: ptr(true), URL: a})
	var res struct {
		Rules []SubRule
		URL   string
	}
	api.OK(t, "POST", "/subscriptions/"+sub.ID+"/sync", map[string]string{"url": b}, &res)
	if res.URL != b || len(res.Rules) != 1 || res.Rules[0].Rule != "b.example.org" {
		t.Errorf("sync with url: %+v", res)
	}
	if got := api.Subscriptions(t)[0].URL; got != b {
		t.Errorf("subscription url = %s, want %s", got, b)
	}
}

func TestSubscriptionRulesPreview(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	url := publishList(t, "preview.txt", "p1.example.org\n192.0.2.0/24\n")
	var res struct{ Rules []SubRule }
	api.OK(t, "GET", "/subscriptions/rules?url="+url, nil, &res)
	if len(res.Rules) != 2 {
		t.Errorf("preview: %+v", res.Rules)
	}
	if c := api.Status(t, "GET", "/subscriptions/rules", nil); c != 400 {
		t.Errorf("no url = %d", c)
	}
	if c := api.Status(t, "GET", "/subscriptions/rules?url="+listURL+"nothing", nil); c != 502 {
		t.Errorf("unreachable = %d", c)
	}
	if got := api.Subscriptions(t); len(got) != 0 {
		t.Errorf("preview created a subscription: %+v", got)
	}
}

func TestSubscriptionFetchFollowsRedirects(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	// busybox httpd-style redirect via a tiny shell server on a second port
	rt.MustPut(t, "/opt/tmp/redir.sh", []byte(`#!/bin/sh
read line
printf 'HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:`+subscribePort+`/redirected.txt\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'
`), 0o755)
	publishList(t, "redirected.txt", "redirect.example.org\n")
	p, err := rt.Spawn("redir", "socat TCP-LISTEN:8098,bind=127.0.0.1,reuseaddr,fork EXEC:/opt/tmp/redir.sh")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { p.Stop(3 * time.Second) })
	time.Sleep(time.Second)

	var res struct{ Rules []SubRule }
	api.OK(t, "GET", "/subscriptions/rules?url=http://127.0.0.1:8098/start", nil, &res)
	if len(res.Rules) != 1 || res.Rules[0].Rule != "redirect.example.org" {
		t.Errorf("preview via redirect: %+v", res.Rules)
	}
}

func TestSubscriptionAutoUpdateOnStartup(t *testing.T) {
	url := publishList(t, "auto.txt", "auto.example.org\n")
	cfg := DaemonConfig{ShowAll: true, Subscriptions: fmt.Sprintf(`  - id: 0a0b0c0d
    name: auto
    interface: eth3
    enable: true
    url: %s
    interval: 3600
    last_update: 0
    rules: []
`, url)}
	d := StartDaemon(t, cfg)
	WaitFor(t, 15*time.Second, "automatic fetch", func() bool {
		s := api.Subscriptions(t)
		return len(s) == 1 && s[0].Rules != nil && len(*s[0].Rules) == 1
	})
	_ = d

	// Not due again: the interval has not elapsed, so a changed list is not picked up on the next start.
	api.Save(t)
	publishList(t, "auto.txt", "auto.example.org\nlater.example.org\n")
	Restart(t)
	time.Sleep(3 * time.Second)
	s := api.Subscriptions(t)
	if len(s) != 1 {
		t.Fatalf("subscriptions after restart: %+v", s)
	}
	if s[0].Rules == nil || len(*s[0].Rules) != 1 {
		t.Errorf("subscription refreshed before its interval elapsed: %+v", s[0].Rules)
	}
}

func TestSubscriptionWithoutIntervalIsNeverAutoUpdated(t *testing.T) {
	url := publishList(t, "manual.txt", "manual.example.org\n")
	StartDaemon(t, DaemonConfig{ShowAll: true, Subscriptions: fmt.Sprintf(`  - id: 0a0b0c0e
    name: manual
    interface: eth3
    enable: true
    url: %s
    interval: 0
    rules: []
`, url)})
	time.Sleep(4 * time.Second)
	s := api.Subscriptions(t)
	if len(s) != 1 {
		t.Fatalf("subscriptions: %+v", s)
	}
	if s[0].Rules != nil && len(*s[0].Rules) != 0 {
		t.Errorf("interval 0 subscription was fetched: %+v", s[0].Rules)
	}
}

func TestSubscriptionPersistsAcrossRestart(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	url := publishList(t, "persist.txt", "persist.example.org\n198.51.100.0/24\n")
	sub := createSub(t, Subscription{Name: "persist", Interface: "eth3", Enable: ptr(true), URL: url, Interval: ptr(uint32(3600))})
	api.OK(t, "POST", "/subscriptions/"+sub.ID+"/sync", nil, nil) // saves the config by default

	Restart(t)
	subs := api.Subscriptions(t)
	if len(subs) != 1 || subs[0].ID != sub.ID || subs[0].Rules == nil || len(*subs[0].Rules) != 2 {
		t.Fatalf("after restart: %+v", subs)
	}
	if subs[0].Interval == nil || *subs[0].Interval != 3600 {
		t.Errorf("interval lost: %+v", subs[0])
	}
	waitMembers(t, subSet(sub.ID, false), "198.51.100.0/24")
}

func TestSubscriptionDeleteAndDisable(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	url := publishList(t, "del.txt", "198.51.100.0/24\n")
	sub := createSub(t, Subscription{Name: "del", Interface: "eth3", Enable: ptr(true), URL: url})
	api.OK(t, "POST", "/subscriptions/"+sub.ID+"/sync", nil, nil)
	waitMembers(t, subSet(sub.ID, false), "198.51.100.0/24")

	cur := api.Subscriptions(t)[0]
	cur.Enable = ptr(false)
	api.OK(t, "PUT", "/subscriptions", map[string]any{"subscriptions": []Subscription{cur}}, nil)
	WaitFor(t, 5*time.Second, "sets removed on disable", func() bool { return !ipsetExists(t, subSet(sub.ID, false)) })

	cur.Enable = ptr(true)
	api.OK(t, "PUT", "/subscriptions", map[string]any{"subscriptions": []Subscription{cur}}, nil)
	waitMembers(t, subSet(sub.ID, false), "198.51.100.0/24")

	api.OK(t, "DELETE", "/subscriptions/"+sub.ID, nil, nil)
	WaitFor(t, 5*time.Second, "sets removed on delete", func() bool { return !ipsetExists(t, subSet(sub.ID, false)) })
	if s := snapshot(t); count(s.mangle4, "sub-"+sub.ID) != 0 {
		t.Errorf("iptables rules left for deleted subscription:\n%s", s)
	}
	if c := api.Status(t, "DELETE", "/subscriptions/"+sub.ID, nil); c != 404 {
		t.Errorf("second delete = %d", c)
	}
}
