package subscriptions

import (
	"time"

	"magitrickle/models"

	"github.com/rs/zerolog/log"
)

func SyncDueSubscriptions(subs []*models.Subscription, now time.Time) (bool, bool) {
	nowSeconds := uint32(now.Unix())
	checked := false
	changed := false

	for _, sub := range subs {
		if sub == nil || !sub.Enable || sub.URL == "" {
			continue
		}
		interval := sub.Interval
		if interval == 0 {
			continue
		}
		lastCheck := sub.LastCheck
		if lastCheck == 0 {
			lastCheck = sub.LastUpdate
		}
		if lastCheck > 0 && uint64(nowSeconds) < uint64(lastCheck)+uint64(interval) {
			continue
		}

		list, err := FetchList(sub.URL)
		if err != nil {
			log.Error().Err(err).Str("subscription", sub.ID.String()).Msg("failed to fetch subscription")
			continue
		}

		checked = true
		if ApplyFetchedRules(sub, list, now) {
			changed = true
		}
	}

	return checked, changed
}

func ApplyFetchedRules(sub *models.Subscription, list string, now time.Time) bool {
	if sub == nil {
		return false
	}

	refreshed := RefreshRules(list, sub.Rules)
	sub.LastCheck = uint32(now.Unix())
	if sameRules(sub.Rules, refreshed) {
		return false
	}

	sub.Rules = refreshed
	sub.LastUpdate = sub.LastCheck
	return true
}
