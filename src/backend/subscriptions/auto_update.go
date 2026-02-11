package subscriptions

import (
	"time"

	"magitrickle/models"

	"github.com/rs/zerolog/log"
)

func SyncDueSubscriptions(subs []*models.Subscription, now time.Time) bool {
	nowMillis := now.UnixMilli()
	updated := false

	for _, sub := range subs {
		if sub == nil || !sub.Enable || sub.URL == "" {
			continue
		}
		interval := sub.Interval
		if interval == 0 {
			continue
		}
		if sub.LastUpdate > 0 && nowMillis < sub.LastUpdate+int64(interval)*1000 {
			continue
		}

		list, err := FetchList(sub.URL)
		if err != nil {
			log.Error().Err(err).Str("subscription", sub.ID.String()).Msg("failed to fetch subscription")
			continue
		}

		sub.Rules = ParseRules(list)
		sub.LastUpdate = now.UnixMilli()
		updated = true
	}

	return updated
}
