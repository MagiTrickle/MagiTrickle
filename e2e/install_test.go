package e2e

import (
	"strings"
	"testing"
	"time"
)

const (
	pidFile   = "/opt/var/run/magitrickle.pid"
	sockFile  = "/opt/var/run/magitrickle.sock"
	initd     = "/opt/etc/init.d/S99magitrickle"
	hookFile  = "/opt/etc/ndm/netfilter.d/100-magitrickle"
	pkgConf   = "/opt/etc/config/magitrickle.conf"
	ipkOnDisk = "/opt/tmp/magitrickle.ipk"
)

func running(t testing.TB) bool {
	t.Helper()
	_, err := rt.Run("pidof magitrickled")
	return err == nil
}

func TestPackageFiles(t *testing.T) {
	StopAll(t)
	for path, mode := range map[string]string{
		daemonBin: "-rwxr-xr-x",
		initd:     "-rwxr-xr-x",
		hookFile:  "-rwxr-xr-x",
	} {
		out := rt.MustRun(t, "ls -l "+path)
		if !strings.HasPrefix(out, mode[:4]) || !strings.Contains(out, "x") {
			t.Errorf("%s: unexpected mode: %s", path, out)
		}
	}
	for _, path := range []string{
		configPath,
		pkgConf,
		"/opt/usr/share/magitrickle/skins/default/index.html",
	} {
		rt.MustRun(t, "test -f "+path)
	}
	if out := rt.MustRun(t, "opkg status magitrickle"); !strings.Contains(out, "installed") {
		t.Errorf("opkg status: %s", out)
	}
	if out := rt.MustRun(t, "opkg files magitrickle"); !strings.Contains(out, hookFile) {
		t.Errorf("hook file is not listed in package: %s", out)
	}
	conf := rt.MustRun(t, "cat /opt/lib/opkg/info/magitrickle.conffiles")
	for _, f := range []string{configPath, pkgConf} {
		if !strings.Contains(conf, f) {
			t.Errorf("%s is not a conffile: %s", f, conf)
		}
	}
	if out := rt.MustRun(t, "head -c 4 "+daemonBin+" | hexdump -C | head -1"); !strings.Contains(out, "7f 45 4c 46") {
		t.Errorf("daemon is not an ELF binary: %s", out)
	}
	// The binary must be runnable on this kernel/libc (kn build tag => needs socat for the hook).
	rt.MustRun(t, "which socat")
}

func TestDefaultConfigIsValid(t *testing.T) {
	// The config shipped in the package must be accepted by the daemon as is.
	StopAll(t)
	pristine := rt.MustRun(t, "tar -xzOf "+ipkOnDisk+" ./data.tar.gz | tar -xzO ./opt/var/lib/magitrickle/config.yaml")
	d := StartDaemonRaw(t, pristine)
	d.WaitLog("Starting HTTP server", 10*time.Second)
	groups := api.Groups(t, true)
	if len(groups) == 0 {
		t.Fatalf("packaged config has no example groups")
	}
}

func TestInitScript(t *testing.T) {
	StopAll(t)
	rt.MustPut(t, configPath, []byte(DaemonConfig{}.YAML()), 0o644)

	out := rt.MustRun(t, initd+" start")
	if !strings.Contains(stripANSI(out), "done") {
		t.Errorf("start output: %q", out)
	}
	WaitFor(t, 10*time.Second, "daemon to start", func() bool { return running(t) })
	WaitFor(t, 10*time.Second, "API", func() bool {
		c, _, err := api.Do("GET", "/groups", nil)
		return err == nil && c == 200
	})
	pid1 := strings.TrimSpace(rt.MustRun(t, "cat "+pidFile))
	if live := strings.TrimSpace(rt.MustRun(t, "pidof magitrickled")); live != pid1 {
		t.Errorf("pid file %q != running pid %q", pid1, live)
	}
	rt.MustRun(t, "test -S "+sockFile)

	// start on a running daemon is a no-op
	if out := stripANSI(rt.MustRun(t, initd+" start")); !strings.Contains(out, "already running") {
		t.Errorf("second start: %q", out)
	}

	rt.MustRun(t, initd+" restart")
	WaitFor(t, 15*time.Second, "restarted daemon", func() bool {
		c, _, err := api.Do("GET", "/groups", nil)
		return err == nil && c == 200
	})
	pid2 := strings.TrimSpace(rt.MustRun(t, "cat "+pidFile))
	if pid1 == pid2 {
		t.Errorf("restart kept the same pid %s", pid1)
	}

	rt.MustRun(t, initd+" stop")
	if running(t) {
		t.Fatalf("daemon still running after stop")
	}
	if _, err := rt.Run("test -e " + pidFile); err == nil {
		t.Errorf("pid file left after stop")
	}
	if _, err := rt.Run("test -e " + sockFile); err == nil {
		t.Errorf("socket left after stop")
	}
}

func TestSecondInstanceRefused(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	out, err := rt.Run(daemonBin)
	if err == nil {
		t.Fatalf("second instance started:\n%s", out)
	}
	if !strings.Contains(stripANSI(out), "already running") {
		t.Errorf("unexpected message: %s", out)
	}
	// The first instance must stay healthy and keep its pid file.
	if d.Exited() {
		t.Fatal("first daemon died")
	}
	if got := strings.TrimSpace(rt.MustRun(t, "cat "+pidFile)); got == "" {
		t.Error("pid file removed by refused second instance")
	}
}

func TestStalePIDFileIsIgnored(t *testing.T) {
	StopAll(t)
	// PID of a live process that is not magitrickled.
	rt.MustRun(t, "echo 1 > "+pidFile)
	StartDaemon(t, DaemonConfig{})
	if got := strings.TrimSpace(rt.MustRun(t, "cat "+pidFile)); got == "1" {
		t.Error("pid file was not rewritten")
	}
}

// postinst reads RESTART_SERVICE_ON_UPDATE from /opt/etc/config/magitrickle.conf.
func TestPostinstRestartOnUpdate(t *testing.T) {
	reinstall := func() {
		t.Helper()
		out, err := rt.Run("opkg install --force-reinstall " + ipkOnDisk + " 2>&1")
		if err != nil {
			t.Fatalf("reinstall: %v\n%s", err, out)
		}
	}
	t.Cleanup(func() {
		rt.Run("echo RESTART_SERVICE_ON_UPDATE=false > " + pkgConf)
		StopAll(t)
	})

	t.Run("false", func(t *testing.T) {
		StopAll(t)
		rt.MustRun(t, "echo RESTART_SERVICE_ON_UPDATE=false > "+pkgConf)
		reinstall()
		time.Sleep(3 * time.Second)
		if running(t) {
			t.Fatal("daemon was started although RESTART_SERVICE_ON_UPDATE=false")
		}
	})

	t.Run("true", func(t *testing.T) {
		StopAll(t)
		rt.MustRun(t, "echo RESTART_SERVICE_ON_UPDATE=true > "+pkgConf)
		reinstall()
		WaitFor(t, 20*time.Second, "daemon started by postinst", func() bool { return running(t) })
	})

	t.Run("true restarts a running daemon", func(t *testing.T) {
		before := strings.TrimSpace(rt.MustRun(t, "pidof magitrickled"))
		reinstall()
		WaitFor(t, 20*time.Second, "daemon restarted by postinst", func() bool {
			out, err := rt.Run("pidof magitrickled")
			return err == nil && strings.TrimSpace(out) != before
		})
	})

	t.Run("missing config defaults to restart", func(t *testing.T) {
		StopAll(t)
		rt.MustRun(t, "rm -f "+pkgConf+" "+pkgConf+"-opkg")
		reinstall()
		WaitFor(t, 20*time.Second, "daemon started by postinst", func() bool { return running(t) })
	})
}

func TestUserConfigSurvivesUpgrade(t *testing.T) {
	StopAll(t)
	rt.MustRun(t, "echo RESTART_SERVICE_ON_UPDATE=false > "+pkgConf)
	marker := DaemonConfig{LogLevel: "warn"}.YAML() + "# e2e-marker\n"
	rt.MustPut(t, configPath, []byte(marker), 0o644)
	rt.MustRun(t, "opkg install --force-reinstall "+ipkOnDisk+" 2>&1")
	got := rt.MustRun(t, "cat "+configPath)
	if !strings.Contains(got, "# e2e-marker") {
		t.Errorf("config.yaml was overwritten by upgrade:\n%s", got)
	}
	if conf := rt.MustRun(t, "cat "+pkgConf); !strings.Contains(conf, "RESTART_SERVICE_ON_UPDATE=false") {
		t.Errorf("package conf was overwritten by upgrade: %s", conf)
	}
}
