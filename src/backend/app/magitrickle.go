package app

import (
	"context"
	"errors"
	"time"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

var (
	ErrGroupIDConflict = errors.New("group id conflict")
	ErrGroupNotFound   = errors.New("group not found")
	ErrRuleIDConflict  = errors.New("rule id conflict")

	ErrSubscriptionConflict = errors.New("subscription id conflict")
	ErrSubscriptionNotFound = errors.New("subscription not found")
	ErrSubscriptionInvalid  = errors.New("subscription invalid")
	ErrSubscriptionFetch    = errors.New("subscription fetch failed")
	ErrSubscriptionChanged  = errors.New("subscription was changed while it was being fetched")
)

type SubscriptionSyncResult struct {
	URL        string
	LastUpdate uint32
	Rules      []*models.SubscriptionRule
}

type Main interface {
	Config() models.AppConfig

	// Группы и подписки, которые возвращает Main, после публикации не
	// изменяются: их можно читать без блокировок, но менять нельзя. Изменения
	// передаются новыми объектами (models.Group.Clone, models.Subscription.Clone)
	// и сразу применяются к маршрутизации; если применить не удалось,
	// изменение откатывается и возвращается ошибка.
	Groups() []*models.Group
	Group(id intID.ID) *models.Group
	ReplaceGroups(groups []*models.Group) error
	AddGroup(group *models.Group) error
	UpdateGroup(id intID.ID, update func(current *models.Group) (*models.Group, error)) (*models.Group, error)
	RemoveGroup(id intID.ID) (bool, error)
	Subscriptions() []*models.Subscription
	ReplaceSubscriptions(subscriptions []*models.Subscription) error
	AddSubscription(subscription *models.Subscription) error
	RemoveSubscriptionByID(id intID.ID) (bool, error)
	SyncSubscriptionByID(id intID.ID, now time.Time, urlOverride string) (result SubscriptionSyncResult, changed bool, err error)
	SyncDueSubscriptions(now time.Time) (bool, error)
	ListInterfaces() ([]models.InterfaceInfo, error)
	LoadConfig() error
	SaveConfig() error
	ForceCommitIPTables() error
	Start(ctx context.Context) (err error)
}
