package netfilterTools

import (
	"net"
	"testing"

	"github.com/vishvananda/netlink"
	"golang.org/x/sys/unix"
)

func TestHasOwnBlackholeRoute(t *testing.T) {
	_, anyV4, _ := net.ParseCIDR("0.0.0.0/0")
	_, anyV6, _ := net.ParseCIDR("::/0")
	_, lan, _ := net.ParseCIDR("192.168.1.0/24")

	tests := map[string]struct {
		routes []netlink.Route
		want   bool
	}{
		"empty table": {nil, false},
		"blackhole default v4": {[]netlink.Route{
			{Type: unix.RTN_BLACKHOLE, Priority: blackholeRouteMetric, Dst: anyV4},
		}, true},
		"blackhole default v6": {[]netlink.Route{
			{Type: unix.RTN_BLACKHOLE, Priority: blackholeRouteMetric, Dst: anyV6},
		}, true},
		"kernel lists the default route without a destination": {[]netlink.Route{
			{Type: unix.RTN_BLACKHOLE, Priority: blackholeRouteMetric},
		}, true},
		"found among other routes": {[]netlink.Route{
			{Type: unix.RTN_UNICAST, Priority: deviceRouteMetric},
			{Type: unix.RTN_BLACKHOLE, Priority: blackholeRouteMetric, Dst: anyV4},
		}, true},
		"other metric": {[]netlink.Route{
			{Type: unix.RTN_BLACKHOLE, Priority: blackholeRouteMetric + 1, Dst: anyV4},
		}, false},
		"not a blackhole": {[]netlink.Route{
			{Type: unix.RTN_UNICAST, Priority: blackholeRouteMetric, Dst: anyV4},
		}, false},
		"blackhole of a specific network": {[]netlink.Route{
			{Type: unix.RTN_BLACKHOLE, Priority: blackholeRouteMetric, Dst: lan},
		}, false},
	}
	for name, tc := range tests {
		t.Run(name, func(t *testing.T) {
			if got := hasOwnBlackholeRoute(tc.routes); got != tc.want {
				t.Errorf("hasOwnBlackholeRoute = %v, want %v", got, tc.want)
			}
		})
	}
}
