package magitrickle

import (
	"context"
	"time"

	"magitrickle/subscriptions"

	"github.com/rs/zerolog/log"
)

const subscriptionAutoUpdateTick = time.Minute

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
	now := time.Now().UnixMilli()
	updated := false

	for _, sub := range a.subscriptions {
		if sub == nil || !sub.Enable || sub.URL == "" {
			continue
		}
		interval := sub.Interval
		if interval == 0 {
			continue
		}
		if sub.LastUpdate > 0 {
			if now < sub.LastUpdate+int64(interval)*1000 {
				continue
			}
		}

		list, err := subscriptions.FetchList(sub.URL)
		if err != nil {
			log.Error().Err(err).Str("subscription", sub.ID.String()).Msg("failed to fetch subscription")
			continue
		}
		fetched := subscriptions.ParseRules(list)
		sub.Rules = fetched
		sub.LastUpdate = time.Now().UnixMilli()
		updated = true
	}

	if !updated {
		return
	}

	a.SyncSubscriptionGroups()
	if err := a.SaveConfig(); err != nil {
		log.Error().Err(err).Msg("failed to save config file")
	}
}
