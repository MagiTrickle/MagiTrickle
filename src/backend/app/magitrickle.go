package app

import (
	"context"
	"net"

	"magitrickle/config"
	"magitrickle/models"
	"magitrickle/utils/intID"
	"magitrickle/utils/netfilterTools"

	"github.com/vishvananda/netlink"
)

type Main interface {
	Config() models.AppConfig
	Groups() []Group
	UserGroups() []Group
	ClearGroups()
	AddGroup(groupModel *models.Group) error
	RemoveGroupByIndex(idx int)
	RemoveGroupByID(id intID.ID) bool
	SyncSubscriptionGroups()
	Subscriptions() []*models.Subscription
	SetSubscriptions(subscriptions []*models.Subscription)
	AddSubscription(subscription *models.Subscription) error
	RemoveSubscriptionByID(id intID.ID) bool
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
	LinkUpHook(event netlink.LinkUpdate) error
}
