package magitrickle

import (
	"context"
	"errors"
	"fmt"
	"slices"
	"time"

	"magitrickle/app"
	"magitrickle/models"
	"magitrickle/subscriptions"
	"magitrickle/utils/intID"

	"github.com/rs/zerolog/log"
)

const subscriptionAutoUpdateTick = time.Minute

// subscriptionRetryDelay — через сколько повторить обновление подписки, если
// новые правила не удалось применить (но не позже её обычного интервала).
// Каждую минуту — слишком часто: список скачивается заново, а через интервал
// — слишком долго для временного сбоя маршрутизации.
const subscriptionRetryDelay = 5 * time.Minute

func (a *App) StartSubscriptionAutoUpdate(ctx context.Context) {
	if a == nil {
		return
	}

	if _, err := a.SyncDueSubscriptions(time.Now()); err != nil {
		log.Error().Err(err).Msg("failed to sync subscriptions")
	}

	ticker := time.NewTicker(subscriptionAutoUpdateTick)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			if _, err := a.SyncDueSubscriptions(time.Now()); err != nil {
				log.Error().Err(err).Msg("failed to sync subscriptions")
			}
		}
	}
}

func (a *App) SyncSubscriptionByID(id intID.ID, now time.Time, urlOverride string) (app.SubscriptionSyncResult, bool, error) {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.RLock()
	idx := subscriptionIndex(a.subscriptions, id)
	if idx < 0 {
		a.stateMu.RUnlock()
		return app.SubscriptionSyncResult{}, false, app.ErrSubscriptionNotFound
	}
	sub := a.subscriptions[idx]
	fetchURL := urlOverride
	if fetchURL == "" {
		fetchURL = sub.URL
	}
	existingRules := sub.Rules
	a.stateMu.RUnlock()

	if fetchURL == "" {
		return app.SubscriptionSyncResult{}, false, app.ErrSubscriptionInvalid
	}

	list, err := subscriptions.FetchList(fetchURL)
	if err != nil {
		return app.SubscriptionSyncResult{}, false, fmt.Errorf("%w: %v", app.ErrSubscriptionFetch, err)
	}

	refreshed, rulesChanged := subscriptions.PlanRefresh(existingRules, list)
	nowSeconds := uint32(now.Unix())

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	idx = subscriptionIndex(a.subscriptions, id)
	if idx < 0 {
		return app.SubscriptionSyncResult{}, false, app.ErrSubscriptionNotFound
	}
	current := a.subscriptions[idx]
	if !sameFetchInput(current, sub) {
		return app.SubscriptionSyncResult{}, false, app.ErrSubscriptionChanged
	}
	urlChanged := current.URL != fetchURL

	updated := current.Clone()
	updated.URL = fetchURL
	updated.LastCheck = nowSeconds
	if rulesChanged {
		updated.Rules = refreshed
		updated.LastUpdate = nowSeconds
	}
	next := slices.Clone(a.subscriptions)
	next[idx] = updated

	if err := a.publishSubscriptionsLocked(next, rulesChanged); err != nil {
		return app.SubscriptionSyncResult{}, true, err
	}
	delete(a.subscriptionRetryAt, id)
	return app.SubscriptionSyncResult{
		URL:        updated.URL,
		LastUpdate: updated.LastUpdate,
		Rules:      updated.Rules,
	}, urlChanged || rulesChanged, nil
}

func (a *App) SyncDueSubscriptions(now time.Time) (bool, error) {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.RLock()
	var due []*models.Subscription
	for _, sub := range a.subscriptions {
		if !subscriptions.IsDue(sub, now) {
			continue
		}
		if retryAt, ok := a.subscriptionRetryAt[sub.ID]; ok && now.Before(retryAt) {
			continue
		}
		due = append(due, sub)
	}
	a.stateMu.RUnlock()

	if len(due) == 0 {
		return false, nil
	}

	plans := make([]subscriptionPlan, 0, len(due))
	for _, sub := range due {
		list, err := subscriptions.FetchList(sub.URL)
		if err != nil {
			log.Error().Err(err).Str("subscription", sub.ID.String()).Msg("failed to fetch subscription")
			continue
		}
		refreshed, changed := subscriptions.PlanRefresh(sub.Rules, list)
		plans = append(plans, subscriptionPlan{
			fetched:   sub,
			id:        sub.ID,
			refreshed: refreshed,
			changed:   changed,
		})
	}

	if len(plans) == 0 {
		return false, nil
	}

	nowSeconds := uint32(now.Unix())

	a.stateMu.Lock()
	var errs []error
	anyChanged := false
	for _, p := range plans {
		idx := subscriptionIndex(a.subscriptions, p.id)
		if idx < 0 {
			delete(a.subscriptionRetryAt, p.id)
			continue
		}
		if !sameFetchInput(a.subscriptions[idx], p.fetched) {
			// Пока скачивали, подписку изменили; обновим её на следующем тике.
			log.Debug().Str("subscription", p.id.String()).Msg("subscription changed while it was being fetched, skipping")
			continue
		}
		updated := a.subscriptions[idx].Clone()
		updated.LastCheck = nowSeconds
		if p.changed {
			updated.Rules = p.refreshed
			updated.LastUpdate = nowSeconds
		}
		next := slices.Clone(a.subscriptions)
		next[idx] = updated

		// Подписки независимы: применяем по одной, чтобы сбой одной не
		// откатывал уже применённые остальные.
		if err := a.publishSubscriptionsLocked(next, p.changed); err != nil {
			errs = append(errs, fmt.Errorf("subscription %s: %w", p.id, err))
			// Правила откатились, и LastCheck вместе с ними: проверка не
			// удалась. Повторяем не каждую минуту, но и не через весь интервал.
			a.scheduleRetry(p.id, updated.Interval, now)
			continue
		}
		delete(a.subscriptionRetryAt, p.id)
		anyChanged = anyChanged || p.changed
	}
	a.stateMu.Unlock()

	if anyChanged {
		if err := a.SaveConfig(); err != nil {
			log.Error().Err(err).Msg("failed to save config file")
		}
	}
	return anyChanged, errors.Join(errs...)
}

// scheduleRetry откладывает следующее обновление подписки на
// subscriptionRetryDelay, но не дальше её обычного интервала. Вызывается
// под stateMu.
func (a *App) scheduleRetry(id intID.ID, interval uint32, now time.Time) {
	if a.subscriptionRetryAt == nil {
		a.subscriptionRetryAt = make(map[intID.ID]time.Time)
	}
	a.subscriptionRetryAt[id] = now.Add(min(subscriptionRetryDelay, time.Duration(interval)*time.Second))
}

// publishSubscriptionsLocked публикует обновлённые подписки. Если правила не
// менялись (обновились только URL и время проверки), маршрутизация не
// затронута и применять её заново незачем.
func (a *App) publishSubscriptionsLocked(subscriptions []*models.Subscription, rulesChanged bool) error {
	if rulesChanged {
		return a.setSubscriptionsLocked(subscriptions)
	}
	a.subscriptions = subscriptions
	return nil
}

// subscriptionPlan — результат скачивания подписки: новые правила и
// изменились ли они.
type subscriptionPlan struct {
	// fetched — версия подписки, по которой скачивали.
	fetched   *models.Subscription
	id        intID.ID
	refreshed []*models.SubscriptionRule
	changed   bool
}

// sameFetchInput сообщает, что результат скачивания подписки fetched ещё
// годится для current: у них тот же URL и те же правила, из которых
// строилось обновление. Остальные поля (имя, интерфейс, включённость) на
// него не влияют и берутся из current. Сравниваются сами объекты правил:
// опубликованные правила неизменяемы, а API переиспользует неизменённые,
// так что сохранение списка подписок из UI не сбрасывает идущие
// скачивания, пока правила и URL подписки не менялись.
func sameFetchInput(current, fetched *models.Subscription) bool {
	return current.URL == fetched.URL && slices.Equal(current.Rules, fetched.Rules)
}

func subscriptionIndex(subs []*models.Subscription, id intID.ID) int {
	return slices.IndexFunc(subs, func(sub *models.Subscription) bool { return sub.ID == id })
}
