package magitrickle

import (
	"context"
	"time"

	"magitrickle/subscriptions"

	"github.com/rs/zerolog/log"
)

const subscriptionAutoUpdateTick = time.Minute

func (a *App) SyncSubscriptionGroups() {
	a.syncSubscriptionGroups()
}

func (a *App) syncSubscriptionGroups() {
	kept := make([]*Group, 0, len(a.groups))
	usedIDs := make(map[[4]byte]struct{}, len(a.groups))

	for _, group := range a.groups {
		if group.Internal {
			_ = group.Disable()
			continue
		}
		kept = append(kept, group)
		usedIDs[group.ID] = struct{}{}
	}
	a.groups = kept

	for _, groupModel := range subscriptions.BuildSubscriptionGroups(a.subscriptions, usedIDs) {
		if err := a.AddGroup(groupModel); err != nil {
			log.Error().Err(err).Str("subscription", groupModel.ID.String()).Msg("failed to add subscription group")
		}
	}
}

func (a *App) StartSubscriptionAutoUpdate(ctx context.Context) {
	if a == nil {
		return
	}

	a.syncDueSubscriptions()

	ticker := time.NewTicker(subscriptionAutoUpdateTick)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			a.syncDueSubscriptions()
		}
	}
}

func (a *App) syncDueSubscriptions() {
	if !subscriptions.SyncDueSubscriptions(a.subscriptions, time.Now()) {
		return
	}

	a.SyncSubscriptionGroups()
	if err := a.SaveConfig(); err != nil {
		log.Error().Err(err).Msg("failed to save config file")
	}
}
