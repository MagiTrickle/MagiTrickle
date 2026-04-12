package subscriptions

import (
	"fmt"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

func BuildSubscriptionGroups(subs []*models.Subscription, usedIDs map[intID.ID]struct{}) []*models.Group {
	groups := make([]*models.Group, 0, len(subs))
	for _, sub := range subs {
		if sub == nil {
			continue
		}

		if isZeroID(sub.GroupID) || idInUse(sub.GroupID, usedIDs) {
			sub.GroupID = nextUniqueID(usedIDs)
		}
		usedIDs[sub.GroupID] = struct{}{}

		groups = append(groups, subscriptionAsGroup(sub))
	}
	return groups
}

func subscriptionAsGroup(sub *models.Subscription) *models.Group {
	enable := sub.Enable
	if sub.Interface == "" {
		enable = false
	}

	rules := make([]*models.Rule, len(sub.Rules))
	for idx, rule := range sub.Rules {
		rules[idx] = &models.Rule{
			ID:     rule.ID,
			Name:   "",
			Type:   rule.Type,
			Rule:   rule.Rule,
			Enable: rule.Enable,
		}
	}

	name := sub.Name
	if name == "" {
		name = fmt.Sprintf("subscription:%s", sub.ID.String())
	}

	return &models.Group{
		ID:        sub.GroupID,
		Name:      name,
		Color:     "#ffffff",
		Interface: sub.Interface,
		Enable:    enable,
		Rules:     rules,
		Internal:  true,
	}
}

func isZeroID(id intID.ID) bool {
	return id == (intID.ID{})
}

func idInUse(id intID.ID, used map[intID.ID]struct{}) bool {
	_, exists := used[id]
	return exists
}

func nextUniqueID(used map[intID.ID]struct{}) intID.ID {
	for {
		candidate := intID.RandomID()
		if !idInUse(candidate, used) {
			return candidate
		}
	}
}
