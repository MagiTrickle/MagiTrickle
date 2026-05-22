package subscriptions

import (
	"time"

	"magitrickle/models"
)

func IsDue(sub *models.Subscription, now time.Time) bool {
	if sub == nil || !sub.Enable || sub.URL == "" || sub.Interval == 0 {
		return false
	}
	lastCheck := sub.LastCheck
	if lastCheck == 0 {
		lastCheck = sub.LastUpdate
	}
	if lastCheck > 0 && uint64(now.Unix()) < uint64(lastCheck)+uint64(sub.Interval) {
		return false
	}
	return true
}

func PlanRefresh(currentRules []*models.SubscriptionRule, list string) (refreshed []*models.SubscriptionRule, changed bool) {
	refreshed = RefreshRules(list, currentRules)
	changed = !sameRules(currentRules, refreshed)
	return refreshed, changed
}
