package app

import (
	"context"
	"errors"
	"time"

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

type SubscriptionSyncResult struct {
	URL        string
	LastUpdate uint32
	Rules      []*models.SubscriptionRule
}

type Main interface {
	Config() models.AppConfig
	UserGroups() []RuleSet
	ClearGroups()
	AddGroup(groupModel *models.Group) error
	RemoveGroupByIndex(idx int)
	RemoveGroupByID(id intID.ID) bool
	SyncSubscriptionRuleSets() error
	WithSubscriptions(fn func([]*models.Subscription))
	ReplaceSubscriptions(subscriptions []*models.Subscription) error
	AddSubscription(subscription *models.Subscription) error
	RemoveSubscriptionByID(id intID.ID) (bool, error)
	SyncSubscriptionByID(id intID.ID, now time.Time, urlOverride string) (result SubscriptionSyncResult, changed bool, err error)
	SyncDueSubscriptions(now time.Time) (bool, error)
	ListInterfaces() ([]models.InterfaceInfo, error)
	DnsOverrider() *netfilterTools.PortRemap
	LoadConfig() error
	SaveConfig() error
	ForceCommitIPTables() error
	Start(ctx context.Context) (err error)
}

type RuleSet interface {
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
