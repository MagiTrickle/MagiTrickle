//go:build linux

package tests

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"testing"
	"time"

	"magitrickle"
	"magitrickle/api/v1/types"
	"magitrickle/config"

	"github.com/vishvananda/netlink"
	"github.com/vishvananda/netlink/nl"
	"github.com/vishvananda/netns"
	"golang.org/x/sys/unix"
)

func TestRoutingE2E(t *testing.T) {
	requireNetfilterEnv(t)

	env := newNetfilterE2EEnv(t, "netfilter-api-e2e")
	enableLoopbacks(t, env)
	setupTopology(t, env)

	stop := startWanServers(t, env)
	defer stop()

	app := newNetfilterE2EApp(t, env)
	ctx, cancel := context.WithCancel(context.Background())
	errCh := startAppInNetns(t, env.routerNS, app, ctx, env.httpPort)
	defer func() {
		cancel()
		waitForAppStop(t, errCh)
	}()

	baseURL := fmt.Sprintf("http://127.0.0.1:%d/api/v1", env.httpPort)

	status, body := apiRequest(t, env.routerNS, http.MethodGet, baseURL+"/auth", nil)
	assertStatusOK(t, status, body)
	assertInterfaces(t, env.routerNS, baseURL)
	assertNoGroups(t, env.routerNS, baseURL)

	tc := routingCase{
		targetIP: "100.64.0.2",
		port:     18080,
	}

	assertDialFails(t, env.clientNS, tc)

	rules := buildSubnetRules()
	status, body = apiRequest(t, env.routerNS, http.MethodPost, baseURL+"/groups", types.GroupReq{
		Name:      "E2E",
		Interface: env.ifWanRouter,
		Enable:    boolPtr(true),
		RulesReq:  types.RulesReq{Rules: &rules},
	})
	if status != http.StatusOK {
		t.Fatalf("POST /groups => %d, want 200. Body: %s", status, string(body))
	}
	group := decodeGroup(t, body)
	groupID := group.ID.String()
	if group.Interface != env.ifWanRouter {
		t.Fatalf("expected interface %s, got %s", env.ifWanRouter, group.Interface)
	}
	assertGroupHasRules(t, group)
	assertGroupDetails(t, env.routerNS, baseURL, groupID)
	assertGroupRules(t, env.routerNS, baseURL, groupID)

	ruleID := createRule(t, env.routerNS, baseURL, groupID)
	assertRuleExists(t, env.routerNS, baseURL, groupID, ruleID)
	updateRule(t, env.routerNS, baseURL, groupID, ruleID)
	deleteRule(t, env.routerNS, baseURL, groupID, ruleID)

	assertDialSucceeds(t, env.clientNS, env.routerNS, env.ifWanRouter, tc)

	tc6 := routingCase{
		targetIP: "2001:db8:1::2",
		port:     18081,
		isIPv6:   true,
	}
	assertDialSucceeds(t, env.clientNS, env.routerNS, env.ifWanRouter, tc6)

	updateSubnetRules(t, env.routerNS, baseURL, groupID, group)
	triggerNetfilterHook(t, env.routerNS, baseURL)
	disableGroup(t, env.routerNS, baseURL, group)
	assertDialFails(t, env.clientNS, tc)
	enableGroup(t, env.routerNS, baseURL, group)
	assertDialSucceeds(t, env.clientNS, env.routerNS, env.ifWanRouter, tc)
	deleteGroup(t, env.routerNS, baseURL, groupID)
	assertDialFails(t, env.clientNS, tc)
}

type netfilterE2EEnv struct {
	routerNS       netns.NsHandle
	clientNS       netns.NsHandle
	wanNS          netns.NsHandle
	defNS          netns.NsHandle
	ifClientRouter string
	ifRouterClient string
	ifWanRouter    string
	ifRouterWan    string
	ifDefRouter    string
	ifRouterDef    string
	httpPort       uint16
	dnsPort        uint16
}

func newNetfilterE2EEnv(t *testing.T, baseName string) netfilterE2EEnv {
	t.Helper()
	origNS := mustNetnsGet(t)

	env := netfilterE2EEnv{
		routerNS:       mustNetnsNewNamed(t, baseName+"-router", origNS),
		clientNS:       mustNetnsNewNamed(t, baseName+"-client", origNS),
		wanNS:          mustNetnsNewNamed(t, baseName+"-wan", origNS),
		defNS:          mustNetnsNewNamed(t, baseName+"-def", origNS),
		ifClientRouter: "r-client0",
		ifRouterClient: "c0",
		ifWanRouter:    "r-wan0",
		ifRouterWan:    "w0",
		ifDefRouter:    "r-def0",
		ifRouterDef:    "d0",
		httpPort:       18088,
		dnsPort:        53535,
	}

	t.Cleanup(func() {
		_ = netns.DeleteNamed(baseName + "-router")
		_ = netns.DeleteNamed(baseName + "-client")
		_ = netns.DeleteNamed(baseName + "-wan")
		_ = netns.DeleteNamed(baseName + "-def")
	})

	return env
}

func enableLoopbacks(t *testing.T, env netfilterE2EEnv) {
	t.Helper()
	for _, ns := range []netns.NsHandle{env.routerNS, env.clientNS, env.wanNS, env.defNS} {
		withNetns(t, ns, func() {
			mustLinkUp(t, "lo")
			mustEnableIPv6(t)
		})
	}
}

func setupTopology(t *testing.T, env netfilterE2EEnv) {
	t.Helper()
	mustCreateVethPair(t, env.routerNS, env.clientNS, env.ifClientRouter, env.ifRouterClient)
	mustCreateVethPair(t, env.routerNS, env.wanNS, env.ifWanRouter, env.ifRouterWan)
	mustCreateVethPair(t, env.routerNS, env.defNS, env.ifDefRouter, env.ifRouterDef)

	withNetns(t, env.routerNS, func() {
		mustAddrAdd(t, env.ifClientRouter, "10.0.0.1/24")
		mustAddrAdd(t, env.ifWanRouter, "100.64.0.1/30")
		mustAddrAdd(t, env.ifDefRouter, "10.0.2.1/24")
		mustRouteReplace(t, nl.FAMILY_V4, "", "10.0.2.2", env.ifDefRouter)
		mustRouteReplace(t, nl.FAMILY_V4, "100.64.0.2/32", "10.0.2.2", env.ifDefRouter)
		mustWriteProc(t, "/proc/sys/net/ipv4/ip_forward", "1\n")
		mustWriteProc(t, "/proc/sys/net/ipv4/conf/all/rp_filter", "0\n")
		mustAddrAdd(t, env.ifClientRouter, "fd00:0:0:1::1/64")
		mustAddrAdd(t, env.ifWanRouter, "2001:db8:1::1/64")
		mustAddrAdd(t, env.ifDefRouter, "fd00:0:0:2::1/64")
		mustRouteReplace(t, nl.FAMILY_V6, "", "fd00:0:0:2::2", env.ifDefRouter)
		mustRouteReplace(t, nl.FAMILY_V6, "2001:db8:1::2/128", "fd00:0:0:2::2", env.ifDefRouter)
		mustWriteProc(t, "/proc/sys/net/ipv6/conf/all/forwarding", "1\n")
	})

	withNetns(t, env.clientNS, func() {
		mustAddrAdd(t, env.ifRouterClient, "10.0.0.2/24")
		mustRouteReplace(t, nl.FAMILY_V4, "", "10.0.0.1", env.ifRouterClient)
		mustAddrAdd(t, env.ifRouterClient, "fd00:0:0:1::2/64")
		mustRouteReplace(t, nl.FAMILY_V6, "", "fd00:0:0:1::1", env.ifRouterClient)
	})

	withNetns(t, env.wanNS, func() {
		mustAddrAdd(t, env.ifRouterWan, "100.64.0.2/30")
		mustRouteReplace(t, nl.FAMILY_V4, "", "100.64.0.1", env.ifRouterWan)
		mustAddrAdd(t, env.ifRouterWan, "2001:db8:1::2/64")
		mustRouteReplace(t, nl.FAMILY_V6, "", "2001:db8:1::1", env.ifRouterWan)
	})

	withNetns(t, env.defNS, func() {
		mustAddrAdd(t, env.ifRouterDef, "10.0.2.2/24")
		mustAddrAdd(t, env.ifRouterDef, "fd00:0:0:2::2/64")
	})
}

func startWanServers(t *testing.T, env netfilterE2EEnv) func() {
	t.Helper()
	stopIPv4 := startTCPServer(t, env.wanNS, "tcp4", "100.64.0.2:18080")
	stopIPv6 := startTCPServer(t, env.wanNS, "tcp6", "[2001:db8:1::2]:18081")
	return func() {
		stopIPv4()
		stopIPv6()
	}
}

func newNetfilterE2EApp(t *testing.T, env netfilterE2EEnv) *magitrickle.App {
	t.Helper()
	app := magitrickle.New()
	cfg := config.Config{
		ConfigVersion: "0.1.0",
		App: &config.App{
			HTTPWeb: &config.HTTPWeb{
				Host: &config.HTTPWebServer{
					Address: strPtr("127.0.0.1"),
					Port:    uint16Ptr(env.httpPort),
				},
			},
			DNSProxy: &config.DNSProxy{
				Host: &config.DNSProxyServer{
					Address: strPtr("::"),
					Port:    uint16Ptr(env.dnsPort),
				},
				DisableRemap53: boolPtr(true),
			},
			Netfilter: &config.Netfilter{
				IPTables: &config.IPTables{
					ChainPrefix: strPtr("MTE2E_"),
				},
				IPSet: &config.IPSet{
					TablePrefix: strPtr("mte2e_"),
				},
				DisableIPv6: boolPtr(false),
			},
			Link: func() *[]string {
				links := []string{env.ifWanRouter}
				return &links
			}(),
		},
	}
	if err := app.ImportConfig(cfg); err != nil {
		t.Fatalf("import config: %v", err)
	}
	return app
}

func startAppInNetns(t *testing.T, ns netns.NsHandle, app *magitrickle.App, ctx context.Context, httpPort uint16) <-chan error {
	t.Helper()
	errCh := make(chan error, 1)
	go func() {
		withNetns(t, ns, func() {
			errCh <- app.Start(ctx)
		})
	}()

	waitForHTTP(t, ns, fmt.Sprintf("http://127.0.0.1:%d/api/v1/auth", httpPort), errCh)
	return errCh
}

func buildSubnetRules() []types.RuleReq {
	rules := []types.RuleReq{
		{
			Name:   "wan",
			Type:   "subnet",
			Rule:   "100.64.0.2/32",
			Enable: true,
		},
	}
	rules = append(rules, types.RuleReq{
		Name:   "wan6",
		Type:   "subnet6",
		Rule:   "2001:db8:1::2/128",
		Enable: true,
	})
	return rules
}

func apiRequest(t *testing.T, ns netns.NsHandle, method, url string, payload any) (int, []byte) {
	t.Helper()
	var data []byte
	if payload != nil {
		var err error
		data, err = json.Marshal(payload)
		if err != nil {
			t.Fatalf("marshal payload: %v", err)
		}
	}
	return doRequestInNetns(t, ns, method, url, data)
}

func waitForHTTP(t *testing.T, ns netns.NsHandle, url string, errCh <-chan error) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if err := tryRequestInNetns(t, ns, http.MethodGet, url); err == nil {
			return
		}
		select {
		case err := <-errCh:
			if err != nil && !errors.Is(err, context.Canceled) {
				t.Fatalf("app start error: %v", err)
			}
		default:
		}
		time.Sleep(150 * time.Millisecond)
	}
	t.Fatalf("timeout waiting for http server")
}

func tryRequestInNetns(t *testing.T, ns netns.NsHandle, method, url string) error {
	t.Helper()
	var err error
	withNetns(t, ns, func() {
		req, reqErr := http.NewRequest(method, url, nil)
		if reqErr != nil {
			err = reqErr
			return
		}
		client := &http.Client{Timeout: 500 * time.Millisecond}
		resp, reqErr := client.Do(req)
		if reqErr != nil {
			err = reqErr
			return
		}
		_ = resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			err = fmt.Errorf("unexpected status %d", resp.StatusCode)
		}
	})
	return err
}

func waitForAppStop(t *testing.T, errCh <-chan error) {
	t.Helper()
	select {
	case <-errCh:
	case <-time.After(2 * time.Second):
		t.Fatalf("timeout waiting for app to stop")
	}
}

func assertInterfaces(t *testing.T, ns netns.NsHandle, baseURL string) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodGet, baseURL+"/system/interfaces", nil)
	if status != http.StatusOK {
		t.Fatalf("GET /system/interfaces => %d, want 200. Body: %s", status, string(body))
	}
	var res types.InterfacesRes
	mustDecodeJSON(t, body, &res)
	for _, iface := range res.Interfaces {
		if iface.ID == "blackhole" {
			return
		}
	}
	t.Fatalf("expected blackhole interface in response")
}

func assertNoGroups(t *testing.T, ns netns.NsHandle, baseURL string) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodGet, baseURL+"/groups?with_rules=true", nil)
	if status != http.StatusOK {
		t.Fatalf("GET /groups => %d, want 200. Body: %s", status, string(body))
	}
	var res types.GroupsRes
	mustDecodeJSON(t, body, &res)
	if res.Groups != nil && len(*res.Groups) != 0 {
		t.Fatalf("expected no groups initially, got %d", len(*res.Groups))
	}
}

func decodeGroup(t *testing.T, body []byte) types.GroupRes {
	t.Helper()
	var group types.GroupRes
	mustDecodeJSON(t, body, &group)
	return group
}

func assertGroupHasRules(t *testing.T, group types.GroupRes) {
	t.Helper()
	if group.Rules == nil || len(*group.Rules) == 0 {
		t.Fatalf("expected rules in created group")
	}
}

func assertGroupDetails(t *testing.T, ns netns.NsHandle, baseURL, groupID string) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodGet, baseURL+"/groups/"+groupID+"?with_rules=true", nil)
	if status != http.StatusOK {
		t.Fatalf("GET /groups/{id} => %d, want 200. Body: %s", status, string(body))
	}
	var res types.GroupRes
	mustDecodeJSON(t, body, &res)
	if res.Rules == nil || len(*res.Rules) == 0 {
		t.Fatalf("expected rules in group response")
	}
}

func assertGroupRules(t *testing.T, ns netns.NsHandle, baseURL, groupID string) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodGet, baseURL+"/groups/"+groupID+"/rules", nil)
	if status != http.StatusOK {
		t.Fatalf("GET /groups/{id}/rules => %d, want 200. Body: %s", status, string(body))
	}
	var res types.RulesRes
	mustDecodeJSON(t, body, &res)
	if res.Rules == nil || len(*res.Rules) == 0 {
		t.Fatalf("expected rules list")
	}
}

func createRule(t *testing.T, ns netns.NsHandle, baseURL, groupID string) string {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodPost, baseURL+"/groups/"+groupID+"/rules", types.RuleReq{
		Name:   "domain",
		Type:   "domain",
		Rule:   "example.net",
		Enable: true,
	})
	if status != http.StatusOK {
		t.Fatalf("POST /groups/{id}/rules => %d, want 200. Body: %s", status, string(body))
	}
	var rule types.RuleRes
	mustDecodeJSON(t, body, &rule)
	return rule.ID.String()
}

func assertRuleExists(t *testing.T, ns netns.NsHandle, baseURL, groupID, ruleID string) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodGet, baseURL+"/groups/"+groupID+"/rules/"+ruleID, nil)
	if status != http.StatusOK {
		t.Fatalf("GET /groups/{id}/rules/{id} => %d, want 200. Body: %s", status, string(body))
	}
}

func updateRule(t *testing.T, ns netns.NsHandle, baseURL, groupID, ruleID string) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodPut, baseURL+"/groups/"+groupID+"/rules/"+ruleID, types.RuleReq{
		Name:   "regex",
		Type:   "regex",
		Rule:   "^test\\d+\\.example\\.com$",
		Enable: true,
	})
	if status != http.StatusOK {
		t.Fatalf("PUT /groups/{id}/rules/{id} => %d, want 200. Body: %s", status, string(body))
	}
}

func deleteRule(t *testing.T, ns netns.NsHandle, baseURL, groupID, ruleID string) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodDelete, baseURL+"/groups/"+groupID+"/rules/"+ruleID, nil)
	if status != http.StatusOK {
		t.Fatalf("DELETE /groups/{id}/rules/{id} => %d, want 200. Body: %s", status, string(body))
	}
}

func updateSubnetRules(t *testing.T, ns netns.NsHandle, baseURL, groupID string, group types.GroupRes) {
	t.Helper()
	subnetRules := make([]types.RuleReq, 0, 2)
	for _, rule := range *group.Rules {
		if rule.Type == "subnet" || rule.Type == "subnet6" {
			rid := rule.ID
			subnetRules = append(subnetRules, types.RuleReq{
				ID:     &rid,
				Name:   rule.Name,
				Type:   rule.Type,
				Rule:   rule.Rule,
				Enable: true,
			})
		}
	}
	if len(subnetRules) == 0 {
		t.Fatalf("expected subnet rules to update")
	}
	status, body := apiRequest(t, ns, http.MethodPut, baseURL+"/groups/"+groupID+"/rules", types.RulesReq{Rules: &subnetRules})
	if status != http.StatusOK {
		t.Fatalf("PUT /groups/{id}/rules => %d, want 200. Body: %s", status, string(body))
	}
}

func triggerNetfilterHook(t *testing.T, ns netns.NsHandle, baseURL string) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodPost, baseURL+"/system/hooks/netfilterd", types.NetfilterDHookReq{
		Type:  "iptables",
		Table: "nat",
	})
	if status != http.StatusOK {
		t.Fatalf("POST /system/hooks/netfilterd => %d, want 200. Body: %s", status, string(body))
	}
}

func disableGroup(t *testing.T, ns netns.NsHandle, baseURL string, group types.GroupRes) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodPut, baseURL+"/groups/"+group.ID.String(), types.GroupReq{
		ID:        &group.ID,
		Name:      group.Name,
		Color:     group.Color,
		Interface: group.Interface,
		Enable:    boolPtr(false),
	})
	if status != http.StatusOK {
		t.Fatalf("PUT /groups/{id} disable => %d, want 200. Body: %s", status, string(body))
	}
}

func enableGroup(t *testing.T, ns netns.NsHandle, baseURL string, group types.GroupRes) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodPut, baseURL+"/groups/"+group.ID.String(), types.GroupReq{
		ID:        &group.ID,
		Name:      group.Name,
		Color:     group.Color,
		Interface: group.Interface,
		Enable:    boolPtr(true),
	})
	if status != http.StatusOK {
		t.Fatalf("PUT /groups/{id} enable => %d, want 200. Body: %s", status, string(body))
	}
}

func deleteGroup(t *testing.T, ns netns.NsHandle, baseURL, groupID string) {
	t.Helper()
	status, body := apiRequest(t, ns, http.MethodDelete, baseURL+"/groups/"+groupID, nil)
	if status != http.StatusOK {
		t.Fatalf("DELETE /groups/{id} => %d, want 200. Body: %s", status, string(body))
	}
}

func doRequestInNetns(t *testing.T, ns netns.NsHandle, method, url string, data []byte) (int, []byte) {
	t.Helper()
	var status int
	var body []byte
	withNetns(t, ns, func() {
		var reader io.Reader
		if data != nil {
			reader = bytes.NewReader(data)
		}

		req, err := http.NewRequest(method, url, reader)
		if err != nil {
			t.Fatalf("http.NewRequest failed: %v", err)
		}
		if data != nil {
			req.Header.Set("Content-Type", "application/json")
		}

		client := &http.Client{Timeout: 5 * time.Second}
		resp, err := client.Do(req)
		if err != nil {
			t.Fatalf("%s %s error: %v", method, url, err)
		}
		defer resp.Body.Close()

		status = resp.StatusCode
		body, _ = io.ReadAll(resp.Body)
	})
	return status, body
}

func assertStatusOK(t *testing.T, status int, body []byte) {
	t.Helper()
	if status != http.StatusOK {
		t.Fatalf("unexpected status %d, want 200. Body: %s", status, string(body))
	}
}

func mustDecodeJSON(t *testing.T, body []byte, v any) {
	t.Helper()
	if err := json.Unmarshal(body, v); err != nil {
		t.Fatalf("JSON decode error: %v", err)
	}
}

func boolPtr(v bool) *bool {
	return &v
}

func strPtr(v string) *string {
	return &v
}

func uint16Ptr(v uint16) *uint16 {
	return &v
}

const (
	netfilterEnv = "NETFILTER_TESTS"
)

type routingCase struct {
	targetIP string
	port     int
	isIPv6   bool
}

func requireNetfilterEnv(t *testing.T) {
	t.Helper()
	if os.Getenv(netfilterEnv) == "" {
		t.Skipf("%s not set", netfilterEnv)
	}
	if os.Geteuid() != 0 {
		t.Fatalf("must run as root to execute netfilter tests")
	}
	for _, bin := range []string{"iptables", "ip", "ipset"} {
		if _, err := exec.LookPath(bin); err != nil {
			t.Fatalf("required binary %s not found: %v", bin, err)
		}
	}
	if _, err := exec.LookPath("ip6tables"); err != nil {
		t.Fatalf("required binary ip6tables not found: %v", err)
	}
}

func mustNetnsGet(t *testing.T) netns.NsHandle {
	t.Helper()
	ns, err := netns.Get()
	if err != nil {
		t.Fatalf("failed to get current netns: %v", err)
	}
	return ns
}

func mustNetnsNewNamed(t *testing.T, name string, orig netns.NsHandle) netns.NsHandle {
	t.Helper()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	ns, err := netns.NewNamed(name)
	if err != nil {
		t.Fatalf("failed to create netns %s: %v", name, err)
	}
	if err := netns.Set(orig); err != nil {
		t.Fatalf("failed to restore netns after create: %v", err)
	}
	return ns
}

func withNetns(t *testing.T, ns netns.NsHandle, fn func()) {
	t.Helper()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	orig, err := netns.Get()
	if err != nil {
		t.Fatalf("failed to get current netns: %v", err)
	}
	if err := netns.Set(ns); err != nil {
		t.Fatalf("failed to set netns: %v", err)
	}
	defer func() {
		if err := netns.Set(orig); err != nil {
			t.Fatalf("failed to restore netns: %v", err)
		}
	}()

	fn()
}

func mustCreateVethPair(t *testing.T, leftNS, rightNS netns.NsHandle, leftName, rightName string) {
	t.Helper()
	withNetns(t, leftNS, func() {
		veth := &netlink.Veth{
			LinkAttrs: netlink.LinkAttrs{Name: leftName},
			PeerName:  rightName,
		}
		if err := netlink.LinkAdd(veth); err != nil {
			t.Fatalf("veth add error: %v", err)
		}
		peer, err := netlink.LinkByName(rightName)
		if err != nil {
			t.Fatalf("veth peer lookup error: %v", err)
		}
		if err := netlink.LinkSetNsFd(peer, int(rightNS)); err != nil {
			t.Fatalf("veth move error: %v", err)
		}
		mustLinkUp(t, leftName)
	})
	withNetns(t, rightNS, func() {
		mustLinkUp(t, rightName)
	})
}

func mustLinkUp(t *testing.T, name string) {
	t.Helper()
	link, err := netlink.LinkByName(name)
	if err != nil {
		t.Fatalf("link %s not found: %v", name, err)
	}
	if err := netlink.LinkSetUp(link); err != nil {
		t.Fatalf("link %s up error: %v", name, err)
	}
}

func mustAddrAdd(t *testing.T, linkName, cidr string) {
	t.Helper()
	link, err := netlink.LinkByName(linkName)
	if err != nil {
		t.Fatalf("link %s not found: %v", linkName, err)
	}
	addr, err := netlink.ParseAddr(cidr)
	if err != nil {
		t.Fatalf("parse addr %s error: %v", cidr, err)
	}
	if err := netlink.AddrAdd(link, addr); err != nil && !errors.Is(err, unix.EEXIST) {
		t.Fatalf("addr add %s on %s error: %v", cidr, linkName, err)
	}
}

func mustRouteReplace(t *testing.T, family int, dst, gw, linkName string) {
	t.Helper()
	link, err := netlink.LinkByName(linkName)
	if err != nil {
		t.Fatalf("link %s not found: %v", linkName, err)
	}
	var dstNet *net.IPNet
	if dst != "" {
		_, dstNet, err = net.ParseCIDR(dst)
		if err != nil {
			t.Fatalf("parse dst %s error: %v", dst, err)
		}
	}
	gwIP := net.ParseIP(gw)
	if gw != "" && gwIP == nil {
		t.Fatalf("parse gw %s error", gw)
	}
	route := netlink.Route{
		LinkIndex: link.Attrs().Index,
		Dst:       dstNet,
		Gw:        gwIP,
		Family:    family,
	}
	if err := netlink.RouteReplace(&route); err != nil {
		t.Fatalf("route replace dst=%s gw=%s error: %v", dst, gw, err)
	}
}

func mustWriteProc(t *testing.T, path, value string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(value), 0644); err != nil {
		t.Fatalf("write %s error: %v", path, err)
	}
}

func mustEnableIPv6(t *testing.T) {
	t.Helper()
	mustWriteProc(t, "/proc/sys/net/ipv6/conf/all/disable_ipv6", "0\n")
	mustWriteProc(t, "/proc/sys/net/ipv6/conf/default/disable_ipv6", "0\n")
	mustWriteProc(t, "/proc/sys/net/ipv6/conf/lo/disable_ipv6", "0\n")
	mustWriteProc(t, "/proc/sys/net/ipv6/conf/all/accept_dad", "0\n")
	mustWriteProc(t, "/proc/sys/net/ipv6/conf/default/accept_dad", "0\n")
	mustWriteProc(t, "/proc/sys/net/ipv6/conf/all/dad_transmits", "0\n")
	mustWriteProc(t, "/proc/sys/net/ipv6/conf/default/dad_transmits", "0\n")
}

func startTCPServer(t *testing.T, ns netns.NsHandle, network, addr string) func() {
	t.Helper()
	errCh := make(chan error, 1)
	ready := make(chan struct{})
	lnCh := make(chan net.Listener, 1)
	stop := make(chan struct{})

	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()

		if err := netns.Set(ns); err != nil {
			errCh <- err
			return
		}
		ln, err := net.Listen(network, addr)
		if err != nil {
			errCh <- err
			return
		}
		lnCh <- ln
		close(ready)

		for {
			conn, err := ln.Accept()
			if err != nil {
				select {
				case <-stop:
					return
				default:
					errCh <- err
					return
				}
			}
			_ = conn.Close()
		}
	}()

	select {
	case err := <-errCh:
		t.Fatalf("server listen error: %v", err)
	case <-ready:
	}

	return func() {
		close(stop)
		select {
		case ln := <-lnCh:
			_ = ln.Close()
		default:
		}
	}
}

func assertDialFails(t *testing.T, clientNS netns.NsHandle, tc routingCase) {
	t.Helper()
	network := "tcp4"
	addr := fmt.Sprintf("%s:%d", tc.targetIP, tc.port)
	if tc.isIPv6 {
		network = "tcp6"
		addr = fmt.Sprintf("[%s]:%d", tc.targetIP, tc.port)
	}
	var dialErr error
	withNetns(t, clientNS, func() {
		dialErr = dialWithTimeout(network, addr)
	})
	if dialErr == nil {
		t.Fatalf("expected dial to fail before rule is applied")
	}
}

func assertDialSucceeds(t *testing.T, clientNS, routerNS netns.NsHandle, ifaceName string, tc routingCase) {
	t.Helper()
	network := "tcp4"
	addr := fmt.Sprintf("%s:%d", tc.targetIP, tc.port)
	if tc.isIPv6 {
		network = "tcp6"
		addr = fmt.Sprintf("[%s]:%d", tc.targetIP, tc.port)
	}

	var before uint64
	withNetns(t, routerNS, func() {
		link, err := netlink.LinkByName(ifaceName)
		if err == nil && link.Attrs().Statistics != nil {
			before = link.Attrs().Statistics.TxPackets
		}
	})

	var dialErr error
	withNetns(t, clientNS, func() {
		dialErr = dialWithTimeout(network, addr)
	})
	if dialErr != nil {
		t.Fatalf("dial error: %v", dialErr)
	}

	withNetns(t, routerNS, func() {
		link, err := netlink.LinkByName(ifaceName)
		if err != nil || link.Attrs().Statistics == nil {
			return
		}
		after := link.Attrs().Statistics.TxPackets
		if after <= before {
			t.Fatalf("expected tx packets to increase on %s (before=%d after=%d)", ifaceName, before, after)
		}
	})
}

func dialWithTimeout(network, addr string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	dialer := &net.Dialer{}
	conn, err := dialer.DialContext(ctx, network, addr)
	if err != nil {
		return err
	}
	_ = conn.Close()
	return nil
}
