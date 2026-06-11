package magitrickle

import (
	"context"
	"errors"
	"fmt"
	"time"

	"magitrickle/app"
	"magitrickle/models"
	"magitrickle/subscriptions"
	"magitrickle/utils/intID"

	"github.com/rs/zerolog/log"
)

const subscriptionAutoUpdateTick = time.Minute

func (a *App) SyncSubscriptionRuleSets() error {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	return a.syncSubscriptionRuleSetsLocked()
}

func (a *App) syncSubscriptionRuleSetsLocked() error {
	var errs []error

	for _, ruleSet := range a.subscriptionRuleSets {
		if err := ruleSet.Disable(); err != nil {
			errs = append(errs, fmt.Errorf("failed to disable subscription rule set %s: %w", ruleSet.IDValue().String(), err))
		}
	}
	a.subscriptionRuleSets = nil

	runtimeRuleSets, err := a.buildSubscriptionRuleSetsLocked()
	if err != nil {
		return errors.Join(append(errs, err)...)
	}
	a.subscriptionRuleSets = runtimeRuleSets

	return errors.Join(errs...)
}

func (a *App) buildSubscriptionRuleSetsLocked() ([]*RuleSet, error) {
	specs := subscriptions.BuildRuntimeRuleSets(a.subscriptions)
	runtimeRuleSets := make([]*RuleSet, 0, len(specs))
	for _, spec := range specs {
		ruleSet, err := newRuleSet(spec, a)
		if err != nil {
			_ = disableRuleSets(runtimeRuleSets)
			return nil, fmt.Errorf("failed to create subscription rule set %s: %w", spec.ID.String(), err)
		}

		runtimeRuleSets = append(runtimeRuleSets, ruleSet)
		if !a.enabled.Load() {
			continue
		}

		if err := ruleSet.Enable(); err != nil {
			_ = disableRuleSets(runtimeRuleSets)
			return nil, fmt.Errorf("failed to enable subscription rule set %s: %w", ruleSet.IDValue().String(), err)
		}
		if err := ruleSet.Sync(); err != nil {
			_ = disableRuleSets(runtimeRuleSets)
			return nil, fmt.Errorf("failed to sync subscription rule set %s: %w", ruleSet.IDValue().String(), err)
		}
	}
	return runtimeRuleSets, nil
}

func disableRuleSets(ruleSets []*RuleSet) error {
	var errs []error
	for _, ruleSet := range ruleSets {
		if err := ruleSet.Disable(); err != nil {
			errs = append(errs, err)
		}
	}
	return errors.Join(errs...)
}

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
	sub := findSubscriptionByID(a.subscriptions, id)
	if sub == nil {
		a.stateMu.RUnlock()
		return app.SubscriptionSyncResult{}, false, app.ErrSubscriptionNotFound
	}
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

	current := findSubscriptionByID(a.subscriptions, id)
	if current == nil {
		return app.SubscriptionSyncResult{}, false, app.ErrSubscriptionNotFound
	}

	urlChanged := current.URL != fetchURL
	prevURL := current.URL
	prevLastCheck := current.LastCheck
	prevLastUpdate := current.LastUpdate
	prevRules := current.Rules

	current.URL = fetchURL
	current.LastCheck = nowSeconds

	if rulesChanged {
		current.Rules = refreshed
		current.LastUpdate = nowSeconds
		if err := a.syncSubscriptionRuleSetsLocked(); err != nil {
			current.URL = prevURL
			current.LastCheck = prevLastCheck
			current.LastUpdate = prevLastUpdate
			current.Rules = prevRules
			if rollbackErr := a.syncSubscriptionRuleSetsLocked(); rollbackErr != nil {
				return app.SubscriptionSyncResult{}, true, errors.Join(err, fmt.Errorf("failed to rollback subscription sync: %w", rollbackErr))
			}
			return app.SubscriptionSyncResult{}, true, err
		}
	}

	return app.SubscriptionSyncResult{
		URL:        current.URL,
		LastUpdate: current.LastUpdate,
		Rules:      current.Rules,
	}, urlChanged || rulesChanged, nil
}

func (a *App) SyncDueSubscriptions(now time.Time) (bool, error) {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	type dueRef struct {
		id            intID.ID
		url           string
		existingRules []*models.SubscriptionRule
	}

	a.stateMu.RLock()
	var due []dueRef
	for _, sub := range a.subscriptions {
		if !subscriptions.IsDue(sub, now) {
			continue
		}
		due = append(due, dueRef{
			id:            sub.ID,
			url:           sub.URL,
			existingRules: sub.Rules,
		})
	}
	a.stateMu.RUnlock()

	if len(due) == 0 {
		return false, nil
	}

	type planResult struct {
		id        intID.ID
		refreshed []*models.SubscriptionRule
		changed   bool
	}
	plans := make([]planResult, 0, len(due))
	for _, item := range due {
		list, err := subscriptions.FetchList(item.url)
		if err != nil {
			log.Error().Err(err).Str("subscription", item.id.String()).Msg("failed to fetch subscription")
			continue
		}
		refreshed, changed := subscriptions.PlanRefresh(item.existingRules, list)
		plans = append(plans, planResult{
			id:        item.id,
			refreshed: refreshed,
			changed:   changed,
		})
	}

	if len(plans) == 0 {
		return false, nil
	}

	nowSeconds := uint32(now.Unix())

	type rollbackEntry struct {
		sub        *models.Subscription
		rules      []*models.SubscriptionRule
		lastCheck  uint32
		lastUpdate uint32
	}

	a.stateMu.Lock()

	var rollback []rollbackEntry
	anyChanged := false

	for _, p := range plans {
		sub := findSubscriptionByID(a.subscriptions, p.id)
		if sub == nil {
			continue
		}
		rollback = append(rollback, rollbackEntry{
			sub:        sub,
			rules:      sub.Rules,
			lastCheck:  sub.LastCheck,
			lastUpdate: sub.LastUpdate,
		})
		sub.LastCheck = nowSeconds
		if p.changed {
			sub.Rules = p.refreshed
			sub.LastUpdate = nowSeconds
			anyChanged = true
		}
	}

	if !anyChanged {
		a.stateMu.Unlock()
		return false, nil
	}

	if err := a.syncSubscriptionRuleSetsLocked(); err != nil {
		for _, r := range rollback {
			r.sub.Rules = r.rules
			r.sub.LastCheck = r.lastCheck
			r.sub.LastUpdate = r.lastUpdate
		}
		rollbackErr := a.syncSubscriptionRuleSetsLocked()
		a.stateMu.Unlock()
		if rollbackErr != nil {
			return true, errors.Join(err, fmt.Errorf("failed to rollback subscription sync: %w", rollbackErr))
		}
		return true, err
	}
	a.stateMu.Unlock()

	if err := a.SaveConfig(); err != nil {
		log.Error().Err(err).Msg("failed to save config file")
	}
	return true, nil
}

func findSubscriptionByID(subs []*models.Subscription, id intID.ID) *models.Subscription {
	for _, sub := range subs {
		if sub != nil && sub.ID == id {
			return sub
		}
	}
	return nil
}
