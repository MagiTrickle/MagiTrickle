package e2e

import (
	"net/http"
	"strings"
	"testing"
)

func TestGroupCRUD(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})

	if got := api.Groups(t, false); len(got) != 0 {
		t.Fatalf("fresh daemon has groups: %+v", got)
	}

	g := api.CreateGroup(t, Group{Name: "first", Color: "#AbCdEf", Interface: "eth3", Enable: ptr(true)})
	if len(g.ID) != 8 {
		t.Errorf("id %q is not 8 hex chars", g.ID)
	}
	if g.Color != "#abcdef" {
		t.Errorf("color = %q, want lower-cased #abcdef", g.Color)
	}

	// Invalid colors fall back to white, absent enable defaults to true.
	g2 := api.CreateGroup(t, Group{Name: "second", Color: "red", Interface: "br0"})
	if g2.Color != "#ffffff" || g2.Enable == nil || !*g2.Enable {
		t.Errorf("defaults not applied: %+v", g2)
	}

	list := api.Groups(t, false)
	if len(list) != 2 || list[0].ID != g.ID || list[1].ID != g2.ID {
		t.Errorf("list order/content wrong: %+v", list)
	}
	if list[0].Rules != nil {
		t.Errorf("rules returned without with_rules")
	}

	g.Name = "renamed"
	g.Interface = "br0"
	g.Enable = ptr(false)
	var upd Group
	api.OK(t, "PUT", "/groups/"+g.ID, g, &upd)
	if upd.Name != "renamed" || upd.Interface != "br0" || *upd.Enable {
		t.Errorf("update not applied: %+v", upd)
	}
	if got := api.Group(t, g.ID); got.Name != "renamed" {
		t.Errorf("GET after PUT: %+v", got)
	}

	api.OK(t, "DELETE", "/groups/"+g.ID, nil, nil)
	if c := api.Status(t, "GET", "/groups/"+g.ID, nil); c != http.StatusNotFound {
		t.Errorf("deleted group GET = %d", c)
	}
	if c := api.Status(t, "DELETE", "/groups/"+g.ID, nil); c != http.StatusNotFound {
		t.Errorf("deleted group DELETE = %d", c)
	}
	if got := api.Groups(t, false); len(got) != 1 {
		t.Errorf("after delete: %+v", got)
	}
}

func TestGroupBadRequests(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	for name, tc := range map[string]struct {
		method, path string
		body         any
		want         int
	}{
		"bad id":              {"GET", "/groups/zzzz", nil, 400},
		"unknown id":          {"GET", "/groups/00000001", nil, 404},
		"put unknown":         {"PUT", "/groups/00000001", Group{Name: "x"}, 404},
		"rules of unknown":    {"GET", "/groups/00000001/rules", nil, 404},
		"put groups no field": {"PUT", "/groups", map[string]any{}, 400},
	} {
		if got := api.Status(t, tc.method, tc.path, tc.body); got != tc.want {
			t.Errorf("%s: %s %s = %d, want %d", name, tc.method, tc.path, got, tc.want)
		}
	}
	g := api.CreateGroup(t, Group{Name: "g", Interface: "eth3"})
	if got := api.Status(t, "GET", "/groups/"+g.ID+"/rules/xyz", nil); got != 400 {
		t.Errorf("bad rule id = %d", got)
	}
	if got := api.Status(t, "GET", "/groups/"+g.ID+"/rules/00000001", nil); got != 404 {
		t.Errorf("unknown rule id = %d", got)
	}
	code, _, _ := api.Do("POST", "/groups", "not json at all")
	if code != 400 {
		t.Errorf("string body = %d", code)
	}
	req, _ := http.NewRequest("POST", mtBase+"/groups", strings.NewReader("{broken"))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 400 {
		t.Errorf("malformed JSON = %d", resp.StatusCode)
	}
}

func TestRuleCRUD(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "rules", Interface: "eth3", Enable: ptr(true)})

	var r Rule
	api.OK(t, "POST", "/groups/"+g.ID+"/rules", Rule{Name: "n", Type: "domain", Rule: "a.crud.test", Enable: true}, &r)
	if r.ID == "" || r.Rule != "a.crud.test" {
		t.Fatalf("created rule: %+v", r)
	}
	var r2 Rule
	api.OK(t, "POST", "/groups/"+g.ID+"/rules", Rule{Name: "n2", Type: "namespace", Rule: "b.crud.test", Enable: false}, &r2)

	var list struct{ Rules []Rule }
	api.OK(t, "GET", "/groups/"+g.ID+"/rules", nil, &list)
	if len(list.Rules) != 2 {
		t.Fatalf("rules = %+v", list.Rules)
	}

	var got Rule
	api.OK(t, "GET", "/groups/"+g.ID+"/rules/"+r.ID, nil, &got)
	if got != r {
		t.Errorf("GET rule = %+v, want %+v", got, r)
	}

	r.Rule = "c.crud.test"
	r.Enable = false
	api.OK(t, "PUT", "/groups/"+g.ID+"/rules/"+r.ID, r, &got)
	if got.Rule != "c.crud.test" || got.Enable {
		t.Errorf("PUT rule = %+v", got)
	}

	// Replace the whole list, keeping one id.
	var put struct{ Rules []Rule }
	api.OK(t, "PUT", "/groups/"+g.ID+"/rules", map[string]any{"rules": []Rule{
		{ID: r2.ID, Name: "kept", Type: "namespace", Rule: "b.crud.test", Enable: true},
		{Name: "new", Type: "wildcard", Rule: "*.crud.test", Enable: true},
	}}, &put)
	if len(put.Rules) != 2 || put.Rules[0].ID != r2.ID || put.Rules[0].Name != "kept" {
		t.Errorf("PUT rules = %+v", put.Rules)
	}

	api.OK(t, "DELETE", "/groups/"+g.ID+"/rules/"+r2.ID, nil, nil)
	if c := api.Status(t, "GET", "/groups/"+g.ID+"/rules/"+r2.ID, nil); c != 404 {
		t.Errorf("deleted rule GET = %d", c)
	}
	api.OK(t, "GET", "/groups/"+g.ID+"/rules", nil, &list)
	if len(list.Rules) != 1 || list.Rules[0].Name != "new" {
		t.Errorf("after delete: %+v", list.Rules)
	}

	withRules := api.Group(t, g.ID)
	if withRules.Rules != nil {
		t.Errorf("rules in group without with_rules")
	}
	var full Group
	api.OK(t, "GET", "/groups/"+g.ID+"?with_rules=true", nil, &full)
	if full.Rules == nil || len(*full.Rules) != 1 {
		t.Errorf("with_rules: %+v", full)
	}
}

func TestPutGroupsReplacesEverything(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	old := api.CreateGroup(t, Group{Name: "old", Interface: "eth3", Enable: ptr(true),
		Rules: &[]Rule{{Name: "r", Type: "subnet", Rule: "192.0.2.1", Enable: true}}})
	keep := api.CreateGroup(t, Group{Name: "keep", Interface: "eth3", Enable: ptr(true)})
	waitMembers(t, ipsetName(old.ID, false), "192.0.2.1")

	var res struct{ Groups []Group }
	api.OK(t, "PUT", "/groups", map[string]any{"groups": []Group{
		{ID: keep.ID, Name: "keep2", Interface: "br0", Enable: ptr(true),
			Rules: &[]Rule{{Name: "r", Type: "subnet", Rule: "192.0.2.2", Enable: true}}},
		{Name: "added", Interface: "eth3", Enable: ptr(true)},
	}}, &res)
	if len(res.Groups) != 2 || res.Groups[0].ID != keep.ID || res.Groups[0].Name != "keep2" {
		t.Fatalf("PUT /groups result: %+v", res.Groups)
	}
	if c := api.Status(t, "GET", "/groups/"+old.ID, nil); c != 404 {
		t.Errorf("old group still exists: %d", c)
	}
	WaitFor(t, 5e9, "old ipset removed", func() bool { return !ipsetExists(t, ipsetName(old.ID, false)) })
	waitMembers(t, ipsetName(keep.ID, false), "192.0.2.2")
	waitMembers(t, ipsetName(res.Groups[1].ID, false))
}

func TestUnknownRoutes(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	if c := api.Status(t, "GET", "/nope", nil); c != 404 {
		t.Errorf("unknown route = %d", c)
	}
	if c := api.Status(t, "PATCH", "/groups", nil); c != 405 {
		t.Errorf("PATCH /groups = %d", c)
	}
}

func TestWebUIIsServed(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	resp, err := http.Get("http://127.0.0.1:8081/")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 || !strings.Contains(resp.Header.Get("Content-Type"), "text/html") {
		t.Errorf("GET / = %d %s", resp.StatusCode, resp.Header.Get("Content-Type"))
	}
}

func TestUnixSocketAPI(t *testing.T) {
	// The UNIX socket serves the same API; used by the Keenetic hook.
	StartDaemon(t, DaemonConfig{ShowAll: true})
	out := rt.MustRun(t, `printf 'GET /api/v1/groups HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | socat - UNIX-CONNECT:`+sockFile)
	if !strings.HasPrefix(out, "HTTP/1.1 200") || !strings.Contains(out, `"groups"`) {
		t.Errorf("unix socket API: %q", out)
	}
}
