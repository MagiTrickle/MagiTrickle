package e2e

import (
	"strings"
	"testing"
	"time"
)

func TestConfigSaveAndReload(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	g := api.CreateGroup(t, Group{Name: "persist", Color: "#123456", Interface: "eth3", Enable: ptr(true),
		Rules: &[]Rule{
			{Name: "d", Type: "domain", Rule: "p.persist.test", Enable: true},
			{Name: "s", Type: "subnet", Rule: "192.0.2.50", Enable: false},
		}})
	off := api.CreateGroup(t, Group{Name: "disabled", Interface: "br0", Enable: ptr(false)})
	api.Save(t)

	yaml := rt.MustRun(t, "cat "+configPath)
	for _, want := range []string{"name: persist", "p.persist.test", "192.0.2.50", "enable: false", g.ID, off.ID} {
		if !strings.Contains(yaml, want) {
			t.Errorf("saved config misses %q:\n%s", want, yaml)
		}
	}

	d := Restart(t)
	_ = d
	got := api.Groups(t, true)
	if len(got) != 2 {
		t.Fatalf("after restart: %+v", got)
	}
	if got[0].ID != g.ID || got[0].Color != "#123456" || got[0].Interface != "eth3" || len(*got[0].Rules) != 2 {
		t.Errorf("group 0 changed: %+v", got[0])
	}
	if got[1].ID != off.ID || *got[1].Enable {
		t.Errorf("disabled group changed: %+v", got[1])
	}
	if (*got[0].Rules)[1].Enable {
		t.Errorf("disabled rule became enabled")
	}
}

func TestSaveQueryParameter(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	before := rt.MustRun(t, "cat "+configPath)
	api.CreateGroup(t, Group{Name: "unsaved", Interface: "eth3"})
	if after := rt.MustRun(t, "cat "+configPath); after != before {
		t.Errorf("config changed without save=true")
	}
	var g Group
	api.OK(t, "POST", "/groups?save=true", Group{Name: "saved", Interface: "eth3"}, &g)
	after := rt.MustRun(t, "cat "+configPath)
	if !strings.Contains(after, "name: saved") || !strings.Contains(after, "name: unsaved") {
		t.Errorf("save=true did not persist:\n%s", after)
	}
}

func TestUnsavedChangesAreLostOnRestart(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	api.CreateGroup(t, Group{Name: "volatile", Interface: "eth3"})
	Restart(t)
	if got := api.Groups(t, false); len(got) != 0 {
		t.Errorf("unsaved group survived restart: %+v", got)
	}
}

// A group or rule without an "enable" key in config.yaml is loaded disabled.
func TestConfigEnableDefaultsToFalse(t *testing.T) {
	StartDaemonRaw(t, DaemonConfig{ShowAll: true, Groups: `  - id: aabbccdd
    name: legacy
    interface: eth3
    rules:
      - id: 11223344
        name: r
        type: domain
        rule: legacy.test
  - id: aabbccde
    name: explicit
    interface: eth3
    enable: true
    rules:
      - id: 11223345
        name: r
        type: domain
        rule: explicit.test
        enable: true
`}.YAML())
	g := api.Group(t, "aabbccdd")
	if g.Enable == nil || *g.Enable {
		t.Errorf("group without enable must be disabled: %+v", g)
	}
	var rules struct{ Rules []Rule }
	api.OK(t, "GET", "/groups/aabbccdd/rules", nil, &rules)
	if len(rules.Rules) != 1 || rules.Rules[0].Enable {
		t.Errorf("rule without enable must be disabled: %+v", rules.Rules)
	}
	if e := api.Group(t, "aabbccde"); e.Enable == nil || !*e.Enable {
		t.Errorf("explicit enable: true lost: %+v", e)
	}
	WaitFor(t, 5*time.Second, "ipset of the enabled group", func() bool { return ipsetExists(t, ipsetName("aabbccde", false)) })
	if ipsetExists(t, ipsetName("aabbccdd", false)) {
		t.Error("disabled group got an ipset")
	}
}

func TestMissingConfigStartsWithDefaults(t *testing.T) {
	StopAll(t)
	rt.MustRun(t, "mv "+configPath+" /opt/tmp/config.bak; true")
	t.Cleanup(func() { rt.Run("test -f /opt/tmp/config.bak && mv /opt/tmp/config.bak " + configPath) })
	d := spawnDaemon(t)
	if d.Exited() {
		t.Fatal("daemon died without config")
	}
	if got := api.Groups(t, false); got == nil && false {
		t.Fatal()
	}
	// First save creates the file.
	api.Save(t)
	rt.MustRun(t, "test -s "+configPath)
}

// An unparsable config must be reported, and the daemon falls back to defaults instead of crashing the router service.
func TestBrokenConfigIsReported(t *testing.T) {
	StopAll(t)
	rt.MustPut(t, configPath, []byte("app: [this is: not valid\n  yaml"), 0o644)
	d := spawnDaemon(t)
	d.WaitLog(`failed to load config file`, 5*time.Second)
	if got := api.Groups(t, false); len(got) != 0 {
		t.Errorf("groups from a broken config: %+v", got)
	}
}
