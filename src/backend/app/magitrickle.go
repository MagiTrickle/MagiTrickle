package app

import (
	"context"
	"errors"
	"net"
	"time"

	"magitrickle/config"
	"magitrickle/models"
	"magitrickle/utils/intID"
	"magitrickle/utils/netfilterTools"

	"github.com/vishvananda/netlink"
)

var (
	ErrSubscriptionConflict = errors.New("subscription id conflict")
	ErrSubscriptionNotFound = errors.New("subscription not found")
	ErrSubscriptionInvalid  = errors.New("subscription invalid")
	ErrSubscriptionFetch    = errors.New("subscription fetch failed")
)

type Main interface {
	Config() models.AppConfig
	Groups() []Group
	UserGroups() []Group
	ClearGroups()
	AddGroup(groupModel *models.Group) error
	RemoveGroupByIndex(idx int)
	RemoveGroupByID(id intID.ID) bool
	SyncSubscriptionGroups() error
	Subscriptions() []*models.Subscription
	ReplaceSubscriptions(subscriptions []*models.Subscription) error
	AddSubscription(subscription *models.Subscription) error
	RemoveSubscriptionByID(id intID.ID) (bool, error)
	SyncSubscriptionByID(id intID.ID, now time.Time) (*models.Subscription, error)
	SyncDueSubscriptions(now time.Time) (bool, error)
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
