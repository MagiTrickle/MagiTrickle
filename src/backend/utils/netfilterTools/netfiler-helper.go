package netfilterTools

import (
	"fmt"

	"magitrickle/utils/iptables"
)

// Metrics of the routes in the routing table of every IPSetToLink. The device route wins while the interface is up;
// the blackhole route keeps traffic from leaking into the main table when it is not, and identifies the table as ours.
const (
	deviceRouteMetric    = 10
	blackholeRouteMetric = 20
)

// minPrefixLen is the shortest chain and ipset prefix accepted. The startup cleanup deletes everything that starts
// with the prefix, so an empty or very short one would take foreign chains and sets with it.
const minPrefixLen = 3

type Helper struct {
	ChainPrefix string
	IpsetPrefix string
	IPTables4   *iptables.IPTables
	IPTables6   *iptables.IPTables

	StartIdx uint32
}

func New(chainPrefix, ipsetPrefix string, disableIPv4, disableIPv6 bool, startIdx uint32) (*Helper, error) {
	if len(chainPrefix) < minPrefixLen {
		return nil, fmt.Errorf("chain prefix %q is too short: at least %d characters required", chainPrefix, minPrefixLen)
	}
	if len(ipsetPrefix) < minPrefixLen {
		return nil, fmt.Errorf("ipset prefix %q is too short: at least %d characters required", ipsetPrefix, minPrefixLen)
	}

	var ipt4, ipt6 *iptables.IPTables

	if !disableIPv4 {
		ipt4 = iptables.NewIPTables(iptables.NewRealIPTables())
	}

	if !disableIPv6 {
		ipt6 = iptables.NewIPTables(iptables.NewRealIP6Tables())
	}

	return &Helper{
		ChainPrefix: chainPrefix,
		IpsetPrefix: ipsetPrefix,
		IPTables4:   ipt4,
		IPTables6:   ipt6,
		StartIdx:    startIdx,
	}, nil
}
