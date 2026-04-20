package subscriptions

import (
	"time"

	"magitrickle/models"

	"github.com/rs/zerolog/log"
)

func SyncDueSubscriptions(subs []*models.Subscription, now time.Time) bool {
	nowSeconds := uint32(now.Unix())
	updated := false

	for _, sub := range subs {
		if sub == nil || !sub.Enable || sub.URL == "" {
			continue
		}
		interval := sub.Interval
		if interval == 0 {
			continue
		}
		if sub.LastUpdate > 0 && uint64(nowSeconds) < uint64(sub.LastUpdate)+uint64(interval) {
			continue
		}

		list, err := FetchList(sub.URL)
		if err != nil {
			log.Error().Err(err).Str("subscription", sub.ID.String()).Msg("failed to fetch subscription")
			continue
		}

		sub.Rules = RefreshRules(list, sub.Rules)
		sub.LastUpdate = uint32(now.Unix())
		updated = true
	}

	return updated
}
