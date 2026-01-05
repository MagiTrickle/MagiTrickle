package app

import (
	"context"
	"net"

	"magitrickle/config"
	"magitrickle/models"
	"magitrickle/utils/netfilterTools"

	"github.com/vishvananda/netlink"
)

type Main interface {
	Config() models.AppConfig
	Groups() []Group
	ClearGroups()
	AddGroup(groupModel *models.Group) error
	RemoveGroupByIndex(idx int)
	ListInterfaces() ([]net.Interface, error)
	DnsOverrider() *netfilterTools.PortRemap
	LoadConfig() error
	SaveConfig() error
	ImportConfig(cfg config.Config) error
	ExportConfig() config.Config
	ForceCommitIPTables() error
	Start(ctx context.Context) (err error)
}

type Group interface {
	Enabled() bool
	Model() *models.Group
	AddIPv4Subnet(subnet netfilterTools.IPv4Subnet, ttl netfilterTools.IPSetTimeout) error
	AddIPv6Subnet(subnet netfilterTools.IPv6Subnet, ttl netfilterTools.IPSetTimeout) error
	DelIPv4Subnet(subnet netfilterTools.IPv4Subnet) error
	DelIPv6Subnet(subnet netfilterTools.IPv6Subnet) error
	ListIPv4Subnets() (map[netfilterTools.IPv4Subnet]netfilterTools.IPSetTimeout, error)
	ListIPv6Subnets() (map[netfilterTools.IPv6Subnet]netfilterTools.IPSetTimeout, error)
	Enable() error
	Disable() error
	Sync() error
	LinkUpdateHook(event netlink.LinkUpdate) error
}
