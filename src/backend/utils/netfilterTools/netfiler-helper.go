package netfilterTools

import (
	"magitrickle/utils/iptables"
)

type Helper struct {
	ChainPrefix string
	IpsetPrefix string
	IPTables4   *iptables.IPTables
	IPTables6   *iptables.IPTables
	MarkTable   string

	StartIdx uint32
}

func New(chainPrefix, ipsetPrefix string, disableIPv4, disableIPv6 bool, startIdx uint32) (*Helper, error) {
	var ipt4, ipt6 *iptables.IPTables

	if !disableIPv4 {
		ipt4 = iptables.NewIPTables(iptables.NewRealIPTables())
	}

	if !disableIPv6 {
		ipt6 = iptables.NewIPTables(iptables.NewRealIP6Tables())
	}

	markTable := "mangle"
	for _, ipt := range []*iptables.IPTables{ipt4, ipt6} {
		if ipt == nil {
			continue
		}
		if !ipt.HasTable("raw") {
			markTable = "mangle"
			break
		}
		markTable = "raw"
	}

	return &Helper{
		ChainPrefix: chainPrefix,
		IpsetPrefix: ipsetPrefix,
		IPTables4:   ipt4,
		IPTables6:   ipt6,
		MarkTable:   markTable,
		StartIdx:    startIdx,
	}, nil
}
