package magitrickle

import (
	"fmt"

	"magitrickle/models"
	"magitrickle/utils/intID"

	"github.com/rs/zerolog/log"
)

func (a *App) SyncSubscriptionGroups() {
	a.syncSubscriptionGroups()
}

func (a *App) syncSubscriptionGroups() {
	kept := make([]*Group, 0, len(a.groups))
	for _, group := range a.groups {
		if group.Internal {
			_ = group.Disable()
			continue
		}
		kept = append(kept, group)
	}
	a.groups = kept

	usedIDs := make(map[[4]byte]struct{}, len(a.groups))
	for _, group := range a.groups {
		usedIDs[group.ID] = struct{}{}
	}

	for _, sub := range a.subscriptions {
		if isZeroID(sub.GroupID) || idInUse(sub.GroupID, usedIDs) {
			sub.GroupID = nextUniqueID(usedIDs)
		}
		usedIDs[sub.GroupID] = struct{}{}

		groupModel := subscriptionAsGroup(sub)
		if err := a.AddGroup(groupModel); err != nil {
			log.Error().Err(err).Str("subscription", sub.ID.String()).Msg("failed to add subscription group")
		}
	}
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

func idInUse(id intID.ID, used map[[4]byte]struct{}) bool {
	_, exists := used[id]
	return exists
}

func nextUniqueID(used map[[4]byte]struct{}) intID.ID {
	for {
		candidate := intID.RandomID()
		if !idInUse(candidate, used) {
			return candidate
		}
	}
}
