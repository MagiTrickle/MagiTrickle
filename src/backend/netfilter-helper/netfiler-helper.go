package netfilterHelper

import (
	"fmt"
	"github.com/coreos/go-iptables/iptables"
)

type NetfilterHelper struct {
	ChainPrefix string
	IpsetPrefix string
	IPTables4   *iptables.IPTables
	IPTables6   *iptables.IPTables

	StartIdx uint32
}

func New(chainPrefix, ipsetPrefix string, disableIPv4, disableIPv6 bool, startIdx uint32) (*NetfilterHelper, error) {
	var err error
	var ipt4, ipt6 *iptables.IPTables

	if !disableIPv4 {
		ipt4, err = iptables.New(iptables.IPFamily(iptables.ProtocolIPv4))
		if err != nil {
			return nil, fmt.Errorf("iptables init fail: %w", err)
		}
	}

	if !disableIPv6 {
		ipt6, err = iptables.New(iptables.IPFamily(iptables.ProtocolIPv6))
		if err != nil {
			return nil, fmt.Errorf("ip6tables init fail: %w", err)
		}
	}

	return &NetfilterHelper{
		ChainPrefix: chainPrefix,
		IpsetPrefix: ipsetPrefix,
		IPTables4:   ipt4,
		IPTables6:   ipt6,
		StartIdx:    startIdx,
	}, nil
}
