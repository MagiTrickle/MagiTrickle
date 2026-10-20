package e2e

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

func routeTable(t testing.TB, v6 bool, table int) string {
	t.Helper()
	ip := "ip"
	if v6 {
		ip = "ip -6"
	}
	out, _ := rt.Run(fmt.Sprintf("%s route show table %d", ip, table))
	return out
}

func newVPN(t *testing.T, id, desc string) {
	t.Helper()
	rci.CreateWireguard(t, id, desc)
	t.Cleanup(func() { rci.DeleteInterface(t, id) })
}

func TestRoutingTableForTunnel(t *testing.T) {
	newVPN(t, "Wireguard5", "E2E VPN")
	StartDaemon(t, DaemonConfig{})
	api.CreateGroup(t, Group{Name: "vpn", Interface: "nwg5", Enable: ptr(true)})

	WaitFor(t, 5*time.Second, "route", func() bool { return strings.Contains(routeTable(t, false, firstTable), "default dev nwg5") })
	v4 := routeTable(t, false, firstTable)
	if !strings.Contains(v4, "default dev nwg5") || !strings.Contains(v4, "metric 10") || !strings.Contains(v4, "blackhole default") || !strings.Contains(v4, "metric 20") {
		t.Errorf("v4 table: %s", v4)
	}
	v6 := routeTable(t, true, firstTable)
	if !strings.Contains(v6, "default dev nwg5") || !strings.Contains(v6, "blackhole default") {
		t.Errorf("v6 table: %s", v6)
	}
}

// Traffic must never leak to the main table when the tunnel is gone: the blackhole route has the higher metric.
func TestBlackholeGroup(t *testing.T) {
	StartDaemon(t, DaemonConfig{})
	g := api.CreateGroup(t, Group{Name: "bh", Interface: "blackhole", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "chains", func() bool { return count(snapshot(t).filter4, g.ID) > 0 })

	table := routeTable(t, false, firstTable)
	if strings.TrimSpace(table) != "blackhole default  metric 20" && !strings.Contains(table, "blackhole default") {
		t.Errorf("table: %q", table)
	}
	if strings.Contains(table, " dev ") {
		t.Errorf("blackhole group has a device route: %q", table)
	}
	s := snapshot(t)
	if containsLine(s.filter4, "ACCEPT") {
		t.Errorf("blackhole group accepts forwarded traffic: %v", s.filter4)
	}
	if !containsLine(s.mangle4, "--set-xmark") {
		t.Errorf("blackhole group does not mark traffic: %v", s.mangle4)
	}
}

func TestGatewayRouteOnEthernetInterface(t *testing.T) {
	// ndm may still be re-applying the WAN after an earlier link toggle.
	var lastOut string
	deadline := time.Now().Add(30 * time.Second)
	for {
		out, err := rt.Run("ip route replace default via 10.0.2.2 dev eth3 metric 500")
		if err == nil {
			break
		}
		lastOut = strings.TrimSpace(out)
		if time.Now().After(deadline) {
			state, _ := rt.Run("ip addr show dev eth3; ip route; ip link show dev eth3")
			t.Fatalf("WAN subnet route: %v: %s\nstate of the router:\n%s", err, lastOut, state)
		}
		time.Sleep(250 * time.Millisecond)
	}
	t.Cleanup(func() { rt.Run("ip route del default via 10.0.2.2 dev eth3 metric 500") })

	StartDaemon(t, DaemonConfig{ShowAll: true})
	api.CreateGroup(t, Group{Name: "wan", Interface: "eth3", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "route with gateway", func() bool {
		return strings.Contains(routeTable(t, false, firstTable), "default via 10.0.2.2 dev eth3")
	})
}

// A tunnel that comes up after the daemon started (VPN reconnect) gets its route back via the netlink watcher.
func TestRouteAppearsWhenInterfaceComesUp(t *testing.T) {
	d := StartDaemon(t, DaemonConfig{})
	api.CreateGroup(t, Group{Name: "late", Interface: "nwg7", Enable: ptr(true)})
	d.WaitLog(`interface not found`, 5*time.Second)
	if strings.Contains(routeTable(t, false, firstTable), "nwg7") {
		t.Fatal("route to a nonexistent interface")
	}

	newVPN(t, "Wireguard7", "Late VPN")
	WaitFor(t, 20*time.Second, "route via nwg7", func() bool {
		return strings.Contains(routeTable(t, false, firstTable), "default dev nwg7")
	})
}

func TestRouteRestoredAfterInterfaceFlap(t *testing.T) {
	settleNDMAfter(t)
	newVPN(t, "Wireguard5", "E2E VPN")
	StartDaemon(t, DaemonConfig{})
	api.CreateGroup(t, Group{Name: "flap", Interface: "nwg5", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "route", func() bool { return strings.Contains(routeTable(t, false, firstTable), "default dev nwg5") })

	rci.SetInterface(t, "Wireguard5", false)
	rt.Run(fmt.Sprintf("ip route flush table %d", firstTable)) // whatever the kernel/ndm did to the table
	rci.SetInterface(t, "Wireguard5", true)
	WaitFor(t, 20*time.Second, "route after link up", func() bool {
		return strings.Contains(routeTable(t, false, firstTable), "default dev nwg5")
	})
}

func TestAddressChangeIsHandled(t *testing.T) {
	newVPN(t, "Wireguard5", "E2E VPN")
	d := StartDaemon(t, DaemonConfig{})
	api.CreateGroup(t, Group{Name: "addr", Interface: "nwg5", Enable: ptr(true)})
	WaitFor(t, 5*time.Second, "route", func() bool { return strings.Contains(routeTable(t, false, firstTable), "default dev nwg5") })

	rt.MustRun(t, "ip addr add 10.99.0.1/24 dev nwg5")
	d.WaitLog(`interface address changed.*nwg5`, 5*time.Second)
	if d.Exited() {
		t.Fatal("daemon died")
	}
	if !strings.Contains(routeTable(t, false, firstTable), "default dev nwg5") {
		t.Errorf("route lost after address change: %s", routeTable(t, false, firstTable))
	}
}

func TestInterfaceListShowsTunnelsWithKeeneticNames(t *testing.T) {
	newVPN(t, "Wireguard5", "E2E VPN")
	StartDaemon(t, DaemonConfig{})

	var res struct {
		Interfaces []struct{ ID, Name string }
	}
	api.OK(t, "GET", "/system/interfaces", nil, &res)
	byID := map[string]string{}
	for _, i := range res.Interfaces {
		byID[i.ID] = i.Name
	}
	if _, ok := byID["blackhole"]; !ok || res.Interfaces[0].ID != "blackhole" {
		t.Errorf("blackhole must come first: %+v", res.Interfaces)
	}
	if byID["nwg5"] != "E2E VPN" {
		t.Errorf("nwg5 alias = %q, want the Keenetic description; list: %+v", byID["nwg5"], res.Interfaces)
	}
	for _, hidden := range []string{"eth3", "br0", "lo", "ra0", "ezcfg0"} {
		if _, ok := byID[hidden]; ok {
			t.Errorf("%s must not be offered as a routing target by default", hidden)
		}
	}
}

func TestInterfaceListShowAll(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	var res struct {
		Interfaces []struct{ ID, Name string }
	}
	api.OK(t, "GET", "/system/interfaces", nil, &res)
	byID := map[string]string{}
	for _, i := range res.Interfaces {
		byID[i.ID] = i.Name
	}
	for _, want := range []string{"blackhole", "eth3", "br0", "lo"} {
		if _, ok := byID[want]; !ok {
			t.Errorf("%s missing with showAllInterfaces", want)
		}
	}
	// Keenetic descriptions for system interfaces come from RCI.
	if byID["br0"] != "Home network" {
		t.Errorf("br0 alias = %q, want 'Home network'", byID["br0"])
	}
	if byID["eth3"] != "Broadband connection" {
		t.Errorf("eth3 alias = %q, want 'Broadband connection'", byID["eth3"])
	}
}

func TestInterfaceListWithoutRCI(t *testing.T) {
	// The alias lookup is best effort: when RCI is unreachable the list is still served.
	StartDaemon(t, DaemonConfig{ShowAll: true})
	rt.MustRun(t, "iptables -I OUTPUT -p tcp --dport 79 -j DROP")
	t.Cleanup(func() { rt.Run("iptables -D OUTPUT -p tcp --dport 79 -j DROP") })
	var res struct {
		Interfaces []struct{ ID, Name string }
	}
	api.OK(t, "GET", "/system/interfaces", nil, &res)
	if len(res.Interfaces) < 2 {
		t.Errorf("interfaces = %+v", res.Interfaces)
	}
}
