package magitrickle

import (
	"errors"
	"fmt"
	"slices"
	"sync"
	"sync/atomic"
	"time"

	"magitrickle/app"
	"magitrickle/constant"
	"magitrickle/internal/interfaces"
	"magitrickle/models"
	"magitrickle/routing"
	"magitrickle/rulesets"
	"magitrickle/utils/dnsMITMProxy"
	"magitrickle/utils/intID"
	"magitrickle/utils/netfilterTools"
	"magitrickle/utils/recordsCache"

	"github.com/rs/zerolog/log"
)

var (
	ErrAlreadyRunning           = errors.New("already running")
	ErrConfigUnsupportedVersion = errors.New("config unsupported version")
	ErrConfigNotLoaded          = errors.New("config file was not loaded")
)

// App – основная структура ядра приложения
type App struct {
	enabled atomic.Bool

	// config — текущие настройки; снимок не меняется, LoadConfig подменяет его целиком.
	config atomic.Pointer[models.AppConfig]
	// configLoaded — конфиг прочитан успешно, и его можно перезаписывать.
	configLoaded atomic.Bool
	// configErr — почему конфиг не загрузился, пока configLoaded не выставлен.
	configErr atomic.Pointer[error]

	// stateMu защищает groups и subscriptions. Любое их изменение
	// завершается commitLocked, который применяет новое состояние к
	// маршрутизации. Опубликованные группы и подписки не изменяются на
	// месте — только заменяются новыми объектами, поэтому снимки из Groups
	// и Subscriptions можно читать без блокировки.
	stateMu sync.RWMutex
	// subscriptionSyncMu не даёт обновлениям подписок идти параллельно. На
	// время скачивания держится только он, а не stateMu, поэтому остальные
	// изменения медленного источника не ждут.
	subscriptionSyncMu sync.Mutex
	configSaveMu       sync.Mutex // serializes SaveConfig
	// subscriptionRetryAt — когда повторить обновление подписки, новые
	// правила которой не удалось применить; защищён stateMu.
	subscriptionRetryAt map[intID.ID]time.Time
	groups              []*models.Group
	subscriptions       []*models.Subscription

	// ruleSets собирает наборы правил из groups и subscriptions; защищён stateMu.
	ruleSets rulesets.Builder

	// routing существует, пока приложение запущено.
	routing atomic.Pointer[routing.Table]

	dnsMITM      *dnsMITMProxy.DNSMITMProxy
	nfHelper     *netfilterTools.Helper
	router       *netfilterTools.Router
	recordsCache *recordsCache.Records
	dnsOverrider *netfilterTools.PortRemap
}

// New создаёт новый экземпляр App. Если конфиг существует, но не читается, не разбирается или имеет неподдерживаемую
// версию, возвращается ошибка: запуск с настройками по умолчанию затёр бы пользовательский конфиг при первом сохранении.
func New() (*App, error) {
	a := &App{}
	defaults := constant.DefaultAppConfig
	a.config.Store(&defaults)
	if err := a.LoadConfig(); err != nil {
		return nil, fmt.Errorf("failed to load config file: %w", err)
	}
	return a, nil
}

// Config возвращает конфигурацию
func (a *App) Config() models.AppConfig {
	return *a.cfg()
}

// cfg возвращает текущие настройки. Снимок только для чтения.
func (a *App) cfg() *models.AppConfig {
	if c := a.config.Load(); c != nil {
		return c
	}
	defaults := constant.DefaultAppConfig
	return &defaults
}

// Groups возвращает пользовательские группы в порядке их применения.
func (a *App) Groups() []*models.Group {
	a.stateMu.RLock()
	defer a.stateMu.RUnlock()
	return slices.Clone(a.groups)
}

// Group возвращает группу по ID или nil.
func (a *App) Group(id intID.ID) *models.Group {
	a.stateMu.RLock()
	defer a.stateMu.RUnlock()
	if idx := groupIndex(a.groups, id); idx >= 0 {
		return a.groups[idx]
	}
	return nil
}

// ReplaceGroups заменяет весь список групп. Группы переходят во владение App.
func (a *App) ReplaceGroups(groups []*models.Group) error {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	return a.setGroupsLocked(slices.Clone(groups))
}

// AddGroup добавляет группу в конец списка.
func (a *App) AddGroup(group *models.Group) error {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	if err := a.setGroupsLocked(append(slices.Clone(a.groups), group)); err != nil {
		return err
	}

	log.Info().
		Str("id", group.ID.String()).
		Str("name", group.Name).
		Msg("added group")
	return nil
}

// UpdateGroup заменяет группу id на то, что вернёт update, сохраняя её место
// в списке. update вызывается под блокировкой с текущей версией группы, так
// что параллельные изменения одной группы не теряются. current менять
// нельзя — нужно вернуть новую группу (например, current.Clone()) с тем же ID.
// Методы App из update вызывать нельзя: блокировка уже взята.
func (a *App) UpdateGroup(id intID.ID, update func(current *models.Group) (*models.Group, error)) (*models.Group, error) {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	idx := groupIndex(a.groups, id)
	if idx < 0 {
		return nil, app.ErrGroupNotFound
	}
	group, err := update(a.groups[idx])
	if err != nil {
		return nil, err
	}
	if group == nil {
		return nil, fmt.Errorf("update of group %s returned no group", id)
	}
	if group.ID != id {
		return nil, fmt.Errorf("group ID cannot be changed")
	}

	next := slices.Clone(a.groups)
	next[idx] = group
	if err := a.setGroupsLocked(next); err != nil {
		return nil, err
	}
	return group, nil
}

// RemoveGroup удаляет группу по ID.
func (a *App) RemoveGroup(id intID.ID) (bool, error) {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	idx := groupIndex(a.groups, id)
	if idx < 0 {
		return false, nil
	}
	return true, a.setGroupsLocked(slices.Delete(slices.Clone(a.groups), idx, idx+1))
}

func (a *App) setGroupsLocked(groups []*models.Group) error {
	if err := validateGroups(groups, a.subscriptions); err != nil {
		return err
	}
	previous := a.groups
	a.groups = groups
	return a.commitLocked(func() { a.groups = previous })
}

// Subscriptions возвращает подписки в порядке их применения.
func (a *App) Subscriptions() []*models.Subscription {
	a.stateMu.RLock()
	defer a.stateMu.RUnlock()
	return slices.Clone(a.subscriptions)
}

// ReplaceSubscriptions заменяет весь список подписок. Подписки переходят во владение App.
func (a *App) ReplaceSubscriptions(subscriptions []*models.Subscription) error {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	if err := a.setSubscriptionsLocked(slices.Clone(subscriptions)); err != nil {
		return err
	}
	// Подписки заменены целиком — отложенные повторы относились к прежним.
	clear(a.subscriptionRetryAt)
	return nil
}

// AddSubscription добавляет подписку, если её ID ещё не занят.
func (a *App) AddSubscription(subscription *models.Subscription) error {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	if err := a.setSubscriptionsLocked(append(slices.Clone(a.subscriptions), subscription)); err != nil {
		return err
	}
	delete(a.subscriptionRetryAt, subscription.ID)
	return nil
}

// RemoveSubscriptionByID удаляет подписку по ID.
func (a *App) RemoveSubscriptionByID(id intID.ID) (bool, error) {
	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	idx := subscriptionIndex(a.subscriptions, id)
	if idx < 0 {
		return false, nil
	}
	if err := a.setSubscriptionsLocked(slices.Delete(slices.Clone(a.subscriptions), idx, idx+1)); err != nil {
		return true, err
	}
	delete(a.subscriptionRetryAt, id)
	return true, nil
}

func (a *App) setSubscriptionsLocked(subscriptions []*models.Subscription) error {
	if err := validateSubscriptions(subscriptions, a.groups); err != nil {
		return err
	}
	previous := a.subscriptions
	a.subscriptions = subscriptions
	return a.commitLocked(func() { a.subscriptions = previous })
}

// commitLocked применяет текущие группы и подписки к маршрутизации. Если
// применить не удалось, restore возвращает прежнее состояние, и оно
// применяется заново, чтобы маршрутизация не осталась в промежуточном виде.
// Пока приложение не запущено, ничего не делает.
func (a *App) commitLocked(restore func()) error {
	table := a.routing.Load()
	if table == nil {
		return nil
	}

	err := table.Apply(a.ruleSets.Build(a.subscriptions, a.groups))
	if err == nil || restore == nil {
		return err
	}

	restore()
	if restoreErr := table.Apply(a.ruleSets.Build(a.subscriptions, a.groups)); restoreErr != nil {
		return errors.Join(err, fmt.Errorf("failed to restore previous state: %w", restoreErr))
	}
	return err
}

// routingRetryInterval — как часто повторять наборы правил, которые не
// удалось применить, если их не повторило изменение групп или подписок.
const routingRetryInterval = time.Minute

// retryRouting повторяет наборы правил, которые не применились в прошлый раз.
func (a *App) retryRouting() {
	table := a.routing.Load()
	if table == nil || !table.Pending() {
		return
	}

	a.stateMu.Lock()
	defer a.stateMu.Unlock()
	// Наборы не менялись, поэтому их ошибки Apply только логирует; успех
	// определяем по Pending.
	if err := a.commitLocked(nil); err != nil {
		log.Warn().Err(err).Msg("failed to retry routing")
		return
	}
	if !table.Pending() {
		log.Info().Msg("previously failed rule sets are applied")
	}
}

// ListInterfaces возвращает список сетевых интерфейсов, доступных для выбора в UI.
func (a *App) ListInterfaces() ([]models.InterfaceInfo, error) {
	return interfaces.List(a.cfg().ShowAllInterfaces)
}

func groupIndex(groups []*models.Group, id intID.ID) int {
	return slices.IndexFunc(groups, func(group *models.Group) bool { return group.ID == id })
}

// validateGroups проверяет, что ID групп уникальны и не заняты подписками,
// а ID правил уникальны внутри каждой группы. ID групп и подписок общие:
// по ID маршрутизация именует ipset и цепочки.
func validateGroups(groups []*models.Group, subscriptions []*models.Subscription) error {
	taken := subscriptionIDs(subscriptions)
	seen := make(map[intID.ID]struct{}, len(groups))
	for _, group := range groups {
		if _, ok := taken[group.ID]; ok {
			return fmt.Errorf("%w: %s is already used by a subscription", app.ErrGroupIDConflict, group.ID)
		}
		if _, ok := seen[group.ID]; ok {
			return fmt.Errorf("%w: duplicate group %s", app.ErrGroupIDConflict, group.ID)
		}
		seen[group.ID] = struct{}{}

		if err := uniqueRuleIDs(group.Rules, func(rule *models.Rule) intID.ID { return rule.ID }); err != nil {
			return fmt.Errorf("group %s: %w", group.ID, err)
		}
	}
	return nil
}

// validateSubscriptions проверяет, что ID подписок уникальны и не заняты
// группами, а ID правил уникальны внутри каждой подписки.
func validateSubscriptions(subscriptions []*models.Subscription, groups []*models.Group) error {
	taken := make(map[intID.ID]struct{}, len(groups))
	for _, group := range groups {
		taken[group.ID] = struct{}{}
	}
	seen := make(map[intID.ID]struct{}, len(subscriptions))
	for _, sub := range subscriptions {
		if _, ok := taken[sub.ID]; ok {
			return fmt.Errorf("%w: %s is already used by a group", app.ErrSubscriptionConflict, sub.ID)
		}
		if _, ok := seen[sub.ID]; ok {
			return fmt.Errorf("%w: duplicate subscription %s", app.ErrSubscriptionConflict, sub.ID)
		}
		seen[sub.ID] = struct{}{}

		if err := uniqueRuleIDs(sub.Rules, func(rule *models.SubscriptionRule) intID.ID { return rule.ID }); err != nil {
			return fmt.Errorf("subscription %s: %w", sub.ID, err)
		}
	}
	return nil
}

func uniqueRuleIDs[R any](rules []*R, id func(*R) intID.ID) error {
	seen := make(map[intID.ID]struct{}, len(rules))
	for _, rule := range rules {
		ruleID := id(rule)
		if _, ok := seen[ruleID]; ok {
			return fmt.Errorf("%w: duplicate rule %s", app.ErrRuleIDConflict, ruleID)
		}
		seen[ruleID] = struct{}{}
	}
	return nil
}

func subscriptionIDs(subscriptions []*models.Subscription) map[intID.ID]struct{} {
	ids := make(map[intID.ID]struct{}, len(subscriptions))
	for _, sub := range subscriptions {
		ids[sub.ID] = struct{}{}
	}
	return ids
}
