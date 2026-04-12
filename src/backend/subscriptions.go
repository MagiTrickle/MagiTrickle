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

func (a *App) SyncSubscriptionGroups() error {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	return a.syncSubscriptionGroupsLocked()
}

func (a *App) syncSubscriptionGroupsLocked() error {
	kept := make([]*Group, 0, len(a.groups))
	usedIDs := make(map[intID.ID]struct{}, len(a.groups))
	var errs []error

	for _, group := range a.groups {
		if group.Internal {
			if err := group.Disable(); err != nil {
				errs = append(errs, fmt.Errorf("failed to disable subscription group %s: %w", group.ID.String(), err))
			}
			continue
		}
		kept = append(kept, group)
		usedIDs[group.ID] = struct{}{}
	}
	a.groups = kept

	for _, groupModel := range subscriptions.BuildSubscriptionGroups(a.subscriptions, usedIDs) {
		if err := a.addGroupLocked(groupModel); err != nil {
			errs = append(errs, fmt.Errorf("failed to add subscription group %s: %w", groupModel.ID.String(), err))
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

func (a *App) SyncSubscriptionByID(id intID.ID, now time.Time) (*models.Subscription, error) {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.RLock()
	target := cloneSubscription(findSubscriptionByID(a.subscriptions, id))
	a.stateMu.RUnlock()
	if target == nil {
		return nil, app.ErrSubscriptionNotFound
	}
	if target.URL == "" {
		return nil, app.ErrSubscriptionInvalid
	}

	list, err := subscriptions.FetchList(target.URL)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", app.ErrSubscriptionFetch, err)
	}

	target.Rules = subscriptions.RefreshRules(list, target.Rules)
	target.LastUpdate = now.UnixMilli()

	a.stateMu.Lock()
	defer a.stateMu.Unlock()

	current := findSubscriptionByID(a.subscriptions, id)
	if current == nil {
		return nil, app.ErrSubscriptionNotFound
	}
	previousRules := cloneSubscriptions([]*models.Subscription{current})[0].Rules
	previousLastUpdate := current.LastUpdate
	current.Rules = target.Rules
	current.LastUpdate = target.LastUpdate
	if err := a.syncSubscriptionGroupsLocked(); err != nil {
		current.Rules = previousRules
		current.LastUpdate = previousLastUpdate
		if rollbackErr := a.syncSubscriptionGroupsLocked(); rollbackErr != nil {
			return nil, errors.Join(err, fmt.Errorf("failed to rollback subscription sync: %w", rollbackErr))
		}
		return nil, err
	}
	return cloneSubscription(current), nil
}

func (a *App) SyncDueSubscriptions(now time.Time) (bool, error) {
	a.subscriptionSyncMu.Lock()
	defer a.subscriptionSyncMu.Unlock()

	a.stateMu.RLock()
	nextSubscriptions := cloneSubscriptions(a.subscriptions)
	a.stateMu.RUnlock()
	if !subscriptions.SyncDueSubscriptions(nextSubscriptions, now) {
		return false, nil
	}

	a.stateMu.Lock()
	previousSubscriptions := a.subscriptions
	a.subscriptions = nextSubscriptions
	err := a.syncSubscriptionGroupsLocked()
	a.stateMu.Unlock()
	if err != nil {
		a.stateMu.Lock()
		a.subscriptions = previousSubscriptions
		rollbackErr := a.syncSubscriptionGroupsLocked()
		a.stateMu.Unlock()
		if rollbackErr != nil {
			return true, errors.Join(err, fmt.Errorf("failed to rollback subscription sync: %w", rollbackErr))
		}
		return true, err
	}
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
