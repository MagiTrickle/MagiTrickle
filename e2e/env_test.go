package e2e

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
)

// Environment variables:
//
//	E2E_REUSE=1   use an already running emulator (see emulator.sh start) instead of booting one
//	E2E_KEEP=1    leave the emulator running after the tests
//	E2E_IPK=path  install this package instead of building one
//	E2E_SKIP_INSTALL=1  keep the package already installed on the router
const (
	sshAddr       = "127.0.0.1:8022"
	daemonBin     = "/opt/bin/magitrickled"
	configPath    = "/opt/var/lib/magitrickle/config.yaml"
	upstreamAddr  = "127.0.0.1:5454"
	subscribePort = "8099"
)

var (
	rt  *Router
	rci *RCI
	api *API

	repoRoot string

	helperDNS, helperHTTP *Process
)

func TestMain(m *testing.M) {
	os.Exit(run(m))
}

func run(m *testing.M) int {
	wd, _ := os.Getwd()
	repoRoot = filepath.Dir(wd)

	ipk := os.Getenv("E2E_IPK")
	if ipk == "" && os.Getenv("E2E_SKIP_INSTALL") == "" {
		var err error
		if ipk, err = buildPackage(); err != nil {
			fmt.Fprintln(os.Stderr, "build package:", err)
			return 1
		}
	}
	helper, err := buildHelper()
	if err != nil {
		fmt.Fprintln(os.Stderr, "build helper:", err)
		return 1
	}

	if os.Getenv("E2E_REUSE") == "" {
		if out, err := exec.Command(filepath.Join(wd, "emulator.sh"), "start").CombinedOutput(); err != nil {
			fmt.Fprintf(os.Stderr, "emulator start: %v\n%s", err, out)
			return 1
		}
		if os.Getenv("E2E_KEEP") == "" {
			defer exec.Command(filepath.Join(wd, "emulator.sh"), "stop").Run()
		}
	}

	rt, err = dialWithRetry(sshAddr, 90*time.Second)
	if err != nil {
		fmt.Fprintln(os.Stderr, "ssh:", err)
		return 1
	}
	rci = newRCI()
	api = newAPI()

	if err := provision(ipk, helper); err != nil {
		fmt.Fprintln(os.Stderr, "provision:", err)
		return 1
	}
	code := m.Run()
	if out := os.Getenv("E2E_COVER_OUT"); out != "" {
		fetchCoverage(out)
	}
	return code
}

// fetchCoverage downloads the GOCOVERDIR counters of the instrumented daemon (go tool covdata textfmt -i=<dir>).
func fetchCoverage(dir string) {
	StopAllQuiet()
	// busybox tar cannot create archives, so move the files one by one.
	list, err := rt.Run("ls /opt/tmp/cov")
	if err != nil {
		fmt.Fprintln(os.Stderr, "coverage fetch:", err)
		return
	}
	_ = os.MkdirAll(dir, 0o755)
	for _, name := range strings.Fields(list) {
		b64, err := rt.Run("base64 /opt/tmp/cov/" + name)
		if err != nil {
			fmt.Fprintln(os.Stderr, "coverage fetch:", name, err)
			continue
		}
		cmd := exec.Command("base64", "-d")
		cmd.Stdin = strings.NewReader(b64)
		data, err := cmd.Output()
		if err != nil {
			fmt.Fprintln(os.Stderr, "coverage decode:", name, err)
			continue
		}
		_ = os.WriteFile(filepath.Join(dir, name), data, 0o644)
	}
}

// StopAllQuiet stops the daemon with SIGTERM so that it flushes its coverage counters.
func StopAllQuiet() {
	rt.Run("for p in $(ps | grep '[S]99magitrickle' | awk '{print $1}'); do kill $p; done; killall magitrickled; sleep 3")
}

func dialWithRetry(addr string, d time.Duration) (*Router, error) {
	deadline := time.Now().Add(d)
	for {
		r, err := dialRouter(addr)
		if err == nil {
			return r, nil
		}
		if time.Now().After(deadline) {
			return nil, err
		}
		time.Sleep(2 * time.Second)
	}
}

func buildPackage() (string, error) {
	const version = "0.0.0-e2e"
	cmd := exec.Command("make", "build", "package",
		"PLATFORM=entware", "TARGET=aarch64-3.10_kn", "GOOS=linux", "GOARCH=arm64", "PKG_VERSION="+version)
	cmd.Dir = repoRoot
	cmd.Stdout, cmd.Stderr = os.Stderr, os.Stderr
	if err := cmd.Run(); err != nil {
		return "", err
	}
	matches, _ := filepath.Glob(filepath.Join(repoRoot, ".build", "magitrickle_"+version+"-*_entware_aarch64-3.10_kn.ipk"))
	if len(matches) == 0 {
		return "", fmt.Errorf("package not found after make")
	}
	return matches[0], nil
}

func buildHelper() (string, error) {
	out := filepath.Join(os.TempDir(), "magitrickle-e2e-helper")
	cmd := exec.Command("go", "build", "-ldflags=-s -w", "-o", out, "./helper")
	cmd.Env = append(os.Environ(), "GOOS=linux", "GOARCH=arm64", "CGO_ENABLED=0")
	if b, err := cmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("%v\n%s", err, b)
	}
	return out, nil
}

// provision installs the package without starting the service and starts the helper processes.
func provision(ipkPath, helperPath string) error {
	helper, err := os.ReadFile(helperPath)
	if err != nil {
		return err
	}
	// Dev builds must not be started by postinst: tests start the daemon themselves to capture its output.
	cmds := []string{
		"mkdir -p /opt/etc/config /opt/tmp /opt/tmp/www",
		"echo RESTART_SERVICE_ON_UPDATE=false > /opt/etc/config/magitrickle.conf",
		"killall magitrickled helper 2>/dev/null; true",
	}
	for _, c := range cmds {
		if out, err := rt.Run(c); err != nil {
			return fmt.Errorf("%s: %v\n%s", c, err, out)
		}
	}
	if err := rt.Put("/opt/tmp/helper", helper, 0o755); err != nil {
		return err
	}
	if cov := os.Getenv("E2E_COVER_BIN"); cov != "" {
		b, err := os.ReadFile(cov)
		if err != nil {
			return err
		}
		rt.Run("rm -rf /opt/tmp/cov; mkdir -p /opt/tmp/cov /opt/tmp/covbin")
		if err := rt.Put("/opt/tmp/covbin/magitrickled", b, 0o755); err != nil {
			return err
		}
	}
	if ipkPath != "" {
		ipk, err := os.ReadFile(ipkPath)
		if err != nil {
			return err
		}
		if err := rt.Put("/opt/tmp/magitrickle.ipk", ipk, 0o644); err != nil {
			return err
		}
		if out, err := rt.Run("opkg install --force-reinstall /opt/tmp/magitrickle.ipk"); err != nil {
			return fmt.Errorf("opkg install: %v\n%s", err, out)
		}
	}
	rt.MustPut(nil, "/opt/tmp/zone.json", []byte("[]"), 0o644)
	var err2 error
	if helperDNS, err2 = rt.Spawn("helper-dns", "/opt/tmp/helper dns "+upstreamAddr+" /opt/tmp/zone.json"); err2 != nil {
		return err2
	}
	if helperHTTP, err2 = rt.Spawn("helper-http", "/opt/tmp/helper http 127.0.0.1:"+subscribePort+" /opt/tmp/www"); err2 != nil {
		return err2
	}
	time.Sleep(time.Second)
	for _, p := range []*Process{helperDNS, helperHTTP} {
		if p.Exited() {
			return fmt.Errorf("helper exited: %s", p.Log.String())
		}
	}
	return nil
}

// DaemonConfig is rendered into config.yaml. Zero values pick test-friendly defaults.
type DaemonConfig struct {
	DisableRemap53 bool
	DisableIPv4    bool
	DisableIPv6    bool
	DisableAAAA    bool // disableDropAAAA: keep AAAA records in responses
	DisableFakePTR bool
	AdditionalTTL  string // e.g. "1h"
	Links          []string
	ShowAll        bool
	Upstream       string // host:port, defaults to the scripted helper DNS
	Auth           bool
	Groups         string // raw YAML list items (already indented under "groups:")
	Subscriptions  string
	LogLevel       string
}

func (c DaemonConfig) YAML() string {
	if c.AdditionalTTL == "" {
		c.AdditionalTTL = "1h"
	}
	if len(c.Links) == 0 {
		c.Links = []string{"br0"}
	}
	if c.Upstream == "" {
		c.Upstream = upstreamAddr
	}
	if c.LogLevel == "" {
		c.LogLevel = "debug"
	}
	host, port, _ := strings.Cut(c.Upstream, ":")
	var links strings.Builder
	for _, l := range c.Links {
		fmt.Fprintf(&links, "    - %s\n", l)
	}
	groups := c.Groups
	if groups == "" {
		groups = "[]"
	} else {
		groups = "\n" + groups
	}
	subs := c.Subscriptions
	if subs == "" {
		subs = "[]"
	} else {
		subs = "\n" + subs
	}
	return fmt.Sprintf(`configVersion: 0.7.0
app:
  httpWeb:
    enabled: true
    auth:
      enabled: false
    host:
      address: "[::]"
      port: 8080
    skin: default
  dnsProxy:
    host:
      address: "[::]"
      port: 3553
    upstream:
      address: %s
      port: %s
    disableRemap53: %t
    disableFakePTR: %t
    disableDropAAAA: %t
    maxIdleConns: 10
    maxConcurrent: 100
    timeout: 5s
  netfilter:
    iptables:
      chainPrefix: MT_
    ipset:
      tablePrefix: mt_
      additionalTTL: %s
    disableIPv4: %t
    disableIPv6: %t
    startMarkTableIndex: 1298229097
  link:
%s  showAllInterfaces: %t
  logLevel: %s
groups: %s
subscriptions: %s
`, host, port, c.DisableRemap53, c.DisableFakePTR, c.DisableAAAA, c.AdditionalTTL, c.DisableIPv4, c.DisableIPv6,
		links.String(), c.ShowAll, c.LogLevel, groups, subs)
}

// Daemon is a magitrickled process started by a test with its output captured.
type Daemon struct {
	*Process
	t testing.TB
}

// Start writes the config, starts the daemon and waits until the HTTP API answers.
func StartDaemon(t testing.TB, cfg DaemonConfig) *Daemon {
	t.Helper()
	return StartDaemonRaw(t, cfg.YAML())
}

func StartDaemonRaw(t testing.TB, yaml string) *Daemon {
	t.Helper()
	StopAll(t)
	rt.MustPut(t, configPath, []byte(yaml), 0o644)
	return spawnDaemon(t)
}

// Restart starts the daemon again with the config left on disk.
func Restart(t testing.TB) *Daemon {
	t.Helper()
	StopAll(t)
	return spawnDaemon(t)
}

func spawnDaemon(t testing.TB) *Daemon {
	t.Helper()
	cmd := daemonBin
	if os.Getenv("E2E_COVER_BIN") != "" {
		// Coverage build uploaded by provision; counters are flushed on a clean exit into /opt/tmp/cov.
		cmd = "env GOCOVERDIR=/opt/tmp/cov /opt/tmp/covbin/magitrickled"
	}
	p, err := rt.Spawn("magitrickled", cmd)
	if err != nil {
		t.Fatal(err)
	}
	d := &Daemon{Process: p, t: t}
	t.Cleanup(func() {
		if !p.Exited() {
			_ = p.Stop(15 * time.Second)
		}
		if t.Failed() {
			t.Logf("---- magitrickled output ----\n%s", stripANSI(p.Log.String()))
		}
	})
	WaitFor(t, 20*time.Second, "magitrickle API", func() bool {
		if p.Exited() {
			t.Fatalf("daemon exited early:\n%s", stripANSI(p.Log.String()))
		}
		code, _, err := api.Do("GET", "/groups", nil)
		return err == nil && (code == 200 || code == 401) // 401: auth enabled
	})
	return d
}

var ansiRe = regexp.MustCompile(`\x1b\[[0-9;?]*[A-Za-z]`)

func stripANSI(s string) string { return ansiRe.ReplaceAllString(s, "") }

// Output returns the daemon's log without color codes.
func (d *Daemon) Output() string { return stripANSI(d.Log.String()) }

// WaitLog waits until the daemon output matches the regexp.
func (d *Daemon) WaitLog(pattern string, timeout time.Duration) {
	d.t.Helper()
	re := regexp.MustCompile(pattern)
	WaitFor(d.t, timeout, "log line /"+pattern+"/", func() bool { return re.MatchString(d.Output()) })
}

// CountLog counts matches of the regexp in the output.
func (d *Daemon) CountLog(pattern string) int {
	return len(regexp.MustCompile(pattern).FindAllString(d.Output(), -1))
}

// StopAll stops any running daemon (including ones started by init.d).
func StopAll(t testing.TB) {
	t.Helper()
	// The init script's boot-time loop (uptime < 300s) would respawn the daemon behind our back.
	rt.Run("for p in $(ps | grep '[S]99magitrickle' | awk '{print $1}'); do kill $p; done")
	if _, err := rt.Run("killall magitrickled 2>/dev/null; for i in 1 2 3 4 5 6 7 8 9 10; do pidof magitrickled >/dev/null || exit 0; sleep 1; done; killall -9 magitrickled"); err != nil {
		t.Logf("stopAll: %v", err)
	}
}

// WaitFor polls cond every 250ms until it is true or the timeout expires.
func WaitFor(t testing.TB, timeout time.Duration, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		if cond() {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("timed out after %s waiting for %s", timeout, what)
		}
		time.Sleep(250 * time.Millisecond)
	}
}
