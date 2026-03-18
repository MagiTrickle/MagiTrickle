package magitrickle

import (
	"errors"
	"fmt"
	"net"
	"slices"
	"sync"
	"sync/atomic"

	"magitrickle/app"
	"magitrickle/constant"
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

	dnsMITM       *dnsMITMProxy.DNSMITMProxy
	nfHelper      *netfilterTools.Helper
	recordsCache  *recordsCache.Records
	groups        []*Group
	dnsOverrider  *netfilterTools.PortRemap
	subscriptions []*models.Subscription
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

// Groups возвращает список групп
func (a *App) Groups() []app.Group {
	groupRefs := a.groupSnapshot()
	groups := make([]app.Group, len(groupRefs))
	for i, g := range groupRefs {
		groups[i] = g
	}
	return groups
}

// UserGroups returns only non-internal groups.
func (a *App) UserGroups() []app.Group {
	groupRefs := a.groupSnapshot()
	list := make([]app.Group, 0, len(groupRefs))
	for _, g := range groupRefs {
		if g.Internal {
			continue
		}
		list = append(list, g)
	}
	return list
}

// ClearGroups отключает все группы и очищает список
func (a *App) ClearGroups() {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	for _, g := range a.groups {
		_ = g.Disable()
	}
	a.groups = nil
}

// AddGroup добавляет новую группу
func (a *App) AddGroup(groupModel *models.Group) error {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	return a.addGroupLocked(groupModel)
}

func (a *App) addGroupLocked(groupModel *models.Group) error {
	for _, group := range a.groups {
		if groupModel.ID == group.ID {
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

	grp, err := NewGroup(groupModel, a)
	if err != nil {
		return fmt.Errorf("failed to create group: %w", err)
	}
	a.groups = append(a.groups, grp)
	removeAdded := func() {
		a.groups = a.groups[:len(a.groups)-1]
	}

	log.Info().
		Str("id", grp.ID.String()).
		Str("name", grp.Name).
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
	a.groups = append(a.groups[:idx], a.groups[idx+1:]...)
}

// RemoveGroupByID removes a group by ID.
func (a *App) RemoveGroupByID(id intID.ID) bool {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	for idx, group := range a.groups {
		if group.ID == id {
			a.groups = append(a.groups[:idx], a.groups[idx+1:]...)
			return true
		}
	}
	return false
}

// Subscriptions returns the current subscriptions list.
func (a *App) Subscriptions() []*models.Subscription {
	a.stateMu.RLock()
	defer a.stateMu.RUnlock()
	return cloneSubscriptions(a.subscriptions)
}

// ReplaceSubscriptions replaces the subscriptions list and rebuilds internal groups.
func (a *App) ReplaceSubscriptions(subscriptions []*models.Subscription) error {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	previous := a.subscriptions
	a.subscriptions = cloneSubscriptions(subscriptions)
	if err := a.syncSubscriptionGroupsLocked(); err != nil {
		a.subscriptions = previous
		if rollbackErr := a.syncSubscriptionGroupsLocked(); rollbackErr != nil {
			return errors.Join(err, fmt.Errorf("failed to rollback subscriptions: %w", rollbackErr))
		}
		return err
	}
	return nil
}

// AddSubscription adds a new subscription if ID is unique and rebuilds internal groups.
func (a *App) AddSubscription(subscription *models.Subscription) error {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	for _, sub := range a.subscriptions {
		if sub.ID == subscription.ID {
			return app.ErrSubscriptionConflict
		}
	}
	next := cloneSubscription(subscription)
	a.subscriptions = append(a.subscriptions, next)
	if err := a.syncSubscriptionGroupsLocked(); err != nil {
		a.subscriptions = a.subscriptions[:len(a.subscriptions)-1]
		if rollbackErr := a.syncSubscriptionGroupsLocked(); rollbackErr != nil {
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
			if err := a.syncSubscriptionGroupsLocked(); err != nil {
				a.subscriptions = append(a.subscriptions[:idx], append([]*models.Subscription{removed}, a.subscriptions[idx:]...)...)
				if rollbackErr := a.syncSubscriptionGroupsLocked(); rollbackErr != nil {
					return true, errors.Join(err, fmt.Errorf("failed to rollback subscriptions: %w", rollbackErr))
				}
				return true, err
			}
			return true, nil
		}
	}
	return false, nil
}

// ListInterfaces возвращает список сетевых интерфейсов, удовлетворяющих заданным критериям
func (a *App) ListInterfaces() ([]net.Interface, error) {
	interfaces, err := net.Interfaces()
	if err != nil {
		return nil, fmt.Errorf("failed to get interfaces: %w", err)
	}

	if a.config.ShowAllInterfaces {
		return interfaces, nil
	}

	var filteredInterfaces []net.Interface
	for _, iface := range interfaces {
		if iface.Flags&net.FlagPointToPoint == 0 || slices.Contains(constant.IgnoredInterfaces, iface.Name) {
			continue
		}
		filteredInterfaces = append(filteredInterfaces, iface)
	}
	return filteredInterfaces, nil
}

// DnsOverrider возвращает dnsOverrider
func (a *App) DnsOverrider() *netfilterTools.PortRemap {
	return a.dnsOverrider
}

func (a *App) groupSnapshot() []*Group {
	a.stateMu.RLock()
	defer a.stateMu.RUnlock()

	list := make([]*Group, len(a.groups))
	copy(list, a.groups)
	return list
}

func cloneSubscriptionRule(rule *models.SubscriptionRule) *models.SubscriptionRule {
	if rule == nil {
		return nil
	}
	clone := *rule
	return &clone
}

func cloneSubscription(sub *models.Subscription) *models.Subscription {
	if sub == nil {
		return nil
	}
	clone := *sub
	if sub.Rules != nil {
		clone.Rules = make([]*models.SubscriptionRule, len(sub.Rules))
		for i, rule := range sub.Rules {
			clone.Rules[i] = cloneSubscriptionRule(rule)
		}
	}
	return &clone
}

func cloneSubscriptions(subs []*models.Subscription) []*models.Subscription {
	if subs == nil {
		return nil
	}
	list := make([]*models.Subscription, len(subs))
	for i, sub := range subs {
		list[i] = cloneSubscription(sub)
	}
	return list
}
