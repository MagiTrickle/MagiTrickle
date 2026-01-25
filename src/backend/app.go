package magitrickle

import (
	"errors"
	"fmt"
	"net"
	"slices"
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
	ErrSubscriptionIDConflict   = errors.New("subscription id conflict")
	ErrConfigUnsupportedVersion = errors.New("config unsupported version")
)

// App – основная структура ядра приложения
type App struct {
	enabled atomic.Bool

	config models.AppConfig

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
	groups := make([]app.Group, len(a.groups))
	for i, g := range a.groups {
		groups[i] = g
	}
	return groups
}

// UserGroups returns only non-internal groups.
func (a *App) UserGroups() []app.Group {
	list := make([]app.Group, 0, len(a.groups))
	for _, g := range a.groups {
		if g.Internal {
			continue
		}
		list = append(list, g)
	}
	return list
}

// ClearGroups отключает все группы и очищает список
func (a *App) ClearGroups() {
	kept := make([]*Group, 0, len(a.groups))
	for _, g := range a.groups {
		if g.Internal {
			kept = append(kept, g)
			continue
		}
		_ = g.Disable()
	}
	a.groups = kept
}

// AddGroup добавляет новую группу
func (a *App) AddGroup(groupModel *models.Group) error {
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

	log.Info().
		Str("id", grp.ID.String()).
		Str("name", grp.Name).
		Msg("added group")

	// если приложение уже запущено – включаем группу и выполняем синхронизацию
	if a.enabled.Load() {
		if err = grp.Enable(); err != nil {
			return fmt.Errorf("failed to enable group: %w", err)
		}
		if err = grp.Sync(); err != nil {
			return fmt.Errorf("failed to sync group: %w", err)
		}
	}
	return nil
}

// RemoveGroupByIndex удаляет группу по индексу
func (a *App) RemoveGroupByIndex(idx int) {
	a.groups = append(a.groups[:idx], a.groups[idx+1:]...)
}

// RemoveGroupByID removes a group by ID.
func (a *App) RemoveGroupByID(id intID.ID) bool {
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
	return a.subscriptions
}

// SetSubscriptions replaces the subscriptions list.
func (a *App) SetSubscriptions(subscriptions []*models.Subscription) {
	a.subscriptions = subscriptions
}

// AddSubscription adds a new subscription if ID is unique.
func (a *App) AddSubscription(subscription *models.Subscription) error {
	for _, sub := range a.subscriptions {
		if sub.ID == subscription.ID {
			return ErrSubscriptionIDConflict
		}
	}
	a.subscriptions = append(a.subscriptions, subscription)
	return nil
}

// RemoveSubscriptionByID removes a subscription by ID.
func (a *App) RemoveSubscriptionByID(id intID.ID) bool {
	for idx, sub := range a.subscriptions {
		if sub.ID == id {
			a.subscriptions = append(a.subscriptions[:idx], a.subscriptions[idx+1:]...)
			return true
		}
	}
	return false
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
