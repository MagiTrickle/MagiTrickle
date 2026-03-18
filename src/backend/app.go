package magitrickle

import (
	"errors"
	"fmt"
	"sync"
	"sync/atomic"

	"magitrickle/app"
	"magitrickle/constant"
	groupruntime "magitrickle/groups"
	"magitrickle/internal/interfaces"
	"magitrickle/models"
	"magitrickle/utils/dnsMITMProxy"
	"magitrickle/utils/intID"
	"magitrickle/utils/netfilterTools"
	"magitrickle/utils/recordsCache"

	"github.com/rs/zerolog/log"
)

var (
	ErrAlreadyRunning           = errors.New("already running")
	ErrGroupIDConflict          = errors.New("group id conflict")
	ErrRuleIDConflict           = errors.New("rule id conflict")
	ErrConfigUnsupportedVersion = errors.New("config unsupported version")
)

// App – основная структура ядра приложения
type App struct {
	enabled atomic.Bool

	config  models.AppConfig
	stateMu sync.RWMutex

	subscriptionSyncMu sync.Mutex

	dnsMITM              *dnsMITMProxy.DNSMITMProxy
	nfHelper             *netfilterTools.Helper
	recordsCache         *recordsCache.Records
	userRuleSets         []*RuleSet
	subscriptionRuleSets []*RuleSet
	dnsOverrider         *netfilterTools.PortRemap
	subscriptions        []*models.Subscription
}

// New создаёт новый экземпляр App
func New() *App {
	a := &App{
		config: constant.DefaultAppConfig,
	}
	if err := a.LoadConfig(); err != nil {
		log.Error().Err(err).Msg("failed to load config file")
	}
	return a
}

// Config возвращает конфигурацию
func (a *App) Config() models.AppConfig {
	return a.config
}

// UserGroups returns only user-defined groups.
func (a *App) UserGroups() []app.RuleSet {
	a.stateMu.RLock()
	defer a.stateMu.RUnlock()
	list := make([]app.RuleSet, len(a.userRuleSets))
	for i, g := range a.userRuleSets {
		list[i] = g
	}
	return list
}

// ClearGroups отключает все группы и очищает список
func (a *App) ClearGroups() {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	for _, g := range a.userRuleSets {
		_ = g.Disable()
	}
	a.userRuleSets = nil
}

// AddGroup добавляет новую группу
func (a *App) AddGroup(groupModel *models.Group) error {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	return a.addGroupLocked(groupModel)
}

func (a *App) addGroupLocked(groupModel *models.Group) error {
	for _, group := range a.userRuleSets {
		if groupModel.ID == group.IDValue() {
			return ErrGroupIDConflict
		}
	}
	// Проверка уникальности rule.ID внутри группы.
	dup := make(map[[4]byte]struct{})
	for _, rule := range groupModel.Rules {
		if _, exists := dup[rule.ID]; exists {
			return ErrRuleIDConflict
		}
		dup[rule.ID] = struct{}{}
	}

	grp, err := NewRuleSet(groupruntime.BuildRuntimeRuleSet(groupModel), a)
	if err != nil {
		return fmt.Errorf("failed to create group: %w", err)
	}
	a.userRuleSets = append(a.userRuleSets, grp)
	removeAdded := func() {
		a.userRuleSets = a.userRuleSets[:len(a.userRuleSets)-1]
	}

	log.Info().
		Str("id", grp.IDValue().String()).
		Str("name", grp.DisplayName()).
		Msg("added group")

	// если приложение уже запущено – включаем группу и выполняем синхронизацию
	if a.enabled.Load() {
		if err = grp.Enable(); err != nil {
			removeAdded()
			return fmt.Errorf("failed to enable group: %w", err)
		}
		if err = grp.Sync(); err != nil {
			_ = grp.Disable()
			removeAdded()
			return fmt.Errorf("failed to sync group: %w", err)
		}
	}
	return nil
}

// RemoveGroupByIndex удаляет группу по индексу
func (a *App) RemoveGroupByIndex(idx int) {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	a.userRuleSets = append(a.userRuleSets[:idx], a.userRuleSets[idx+1:]...)
}

// RemoveGroupByID removes a group by ID.
func (a *App) RemoveGroupByID(id intID.ID) bool {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	for idx, group := range a.userRuleSets {
		if group.IDValue() == id {
			a.userRuleSets = append(a.userRuleSets[:idx], a.userRuleSets[idx+1:]...)
			return true
		}
	}
	return false
}

// WithSubscriptions invokes fn while holding the state read lock so the slice
// passed in is a stable snapshot of the live subscriptions. Callers must treat
// the slice and its elements as read-only and must not retain references past
// the callback.
func (a *App) WithSubscriptions(fn func([]*models.Subscription)) {
	a.stateMu.RLock()
	defer a.stateMu.RUnlock()
	fn(a.subscriptions)
}

// ReplaceSubscriptions replaces the subscriptions list and rebuilds subscription rule sets.
func (a *App) ReplaceSubscriptions(subscriptions []*models.Subscription) error {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	previous := a.subscriptions
	a.subscriptions = subscriptions
	if err := a.syncSubscriptionRuleSetsLocked(); err != nil {
		a.subscriptions = previous
		if rollbackErr := a.syncSubscriptionRuleSetsLocked(); rollbackErr != nil {
			return errors.Join(err, fmt.Errorf("failed to rollback subscriptions: %w", rollbackErr))
		}
		return err
	}
	return nil
}

// AddSubscription adds a new subscription if ID is unique and rebuilds subscription rule sets.
func (a *App) AddSubscription(subscription *models.Subscription) error {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	for _, sub := range a.subscriptions {
		if sub.ID == subscription.ID {
			return errors.New("subscription id conflict")
		}
	}
	a.subscriptions = append(a.subscriptions, subscription)
	if err := a.syncSubscriptionRuleSetsLocked(); err != nil {
		a.subscriptions = a.subscriptions[:len(a.subscriptions)-1]
		if rollbackErr := a.syncSubscriptionRuleSetsLocked(); rollbackErr != nil {
			return errors.Join(err, fmt.Errorf("failed to rollback subscriptions: %w", rollbackErr))
		}
		return err
	}
	return nil
}

// RemoveSubscriptionByID removes a subscription by ID.
func (a *App) RemoveSubscriptionByID(id intID.ID) (bool, error) {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	for idx, sub := range a.subscriptions {
		if sub.ID == id {
			removed := sub
			a.subscriptions = append(a.subscriptions[:idx], a.subscriptions[idx+1:]...)
			if err := a.syncSubscriptionRuleSetsLocked(); err != nil {
				a.subscriptions = append(a.subscriptions[:idx], append([]*models.Subscription{removed}, a.subscriptions[idx:]...)...)
				if rollbackErr := a.syncSubscriptionRuleSetsLocked(); rollbackErr != nil {
					return true, errors.Join(err, fmt.Errorf("failed to rollback subscriptions: %w", rollbackErr))
				}
				return true, err
			}
			return true, nil
		}
	}
	return false, nil
}

// ListInterfaces возвращает список сетевых интерфейсов, доступных для выбора в UI.
func (a *App) ListInterfaces() ([]models.InterfaceInfo, error) {
	return interfaces.List(a.config.ShowAllInterfaces)
}

// DnsOverrider возвращает dnsOverrider
func (a *App) DnsOverrider() *netfilterTools.PortRemap {
	return a.dnsOverrider
}

func (a *App) ruleSetSnapshot() []*RuleSet {
	a.stateMu.RLock()
	defer a.stateMu.RUnlock()

	list := make([]*RuleSet, 0, len(a.userRuleSets)+len(a.subscriptionRuleSets))
	list = append(list, a.userRuleSets...)
	list = append(list, a.subscriptionRuleSets...)
	return list
}
