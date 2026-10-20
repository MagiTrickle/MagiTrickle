package v1

import (
	"fmt"

	"magitrickle/api/v1/types"
	"magitrickle/models"
	"magitrickle/utils/intID"
)

func SubscriptionFromReq(req types.SubscriptionReq, existing *models.Subscription) (*models.Subscription, error) {
	sub := &models.Subscription{}
	if existing != nil {
		if req.ID != nil && !req.ID.IsZero() && *req.ID != existing.ID {
			return nil, fmt.Errorf("subscription ID mismatch")
		}
		sub.ID = existing.ID
		sub.LastUpdate = existing.LastUpdate
		sub.LastCheck = existing.LastCheck
	} else {
		sub.ID = idFromReq(req.ID)
	}
	sub.Name = req.Name
	sub.Interface = req.Interface
	sub.URL = req.URL
	sub.Enable = true
	if req.Enable != nil {
		sub.Enable = *req.Enable
	}
	if req.Interval != nil {
		sub.Interval = *req.Interval
	}
	if req.LastUpdate != nil {
		sub.LastUpdate = *req.LastUpdate
	}

	if req.Rules != nil {
		existingRules := make(map[intID.ID]*models.SubscriptionRule)
		if existing != nil {
			for _, rule := range existing.Rules {
				existingRules[rule.ID] = rule
			}
		}
		newRules := make([]*models.SubscriptionRule, len(*req.Rules))
		for i, ruleReq := range *req.Rules {
			newRules[i] = SubscriptionRuleFromReq(ruleReq, existingRules)
		}
		sub.Rules = newRules
	} else if existing != nil {
		sub.Rules = existing.Rules
	}

	return sub, nil
}

// SubscriptionRuleFromReq строит правило подписки из запроса. ID берётся из
// запроса, как у правил групп (см. idFromReq); правило, у которого ничего не
// изменилось, берётся прежним объектом (см. RulesFromReq).
func SubscriptionRuleFromReq(ruleReq types.SubscriptionRuleReq, existingRules map[intID.ID]*models.SubscriptionRule) *models.SubscriptionRule {
	var existing *models.SubscriptionRule
	if ruleReq.ID != nil {
		existing = existingRules[*ruleReq.ID]
	}
	if existing != nil && existing.Rule == ruleReq.Rule && existing.Type == ruleReq.Type && existing.Enable == ruleReq.Enable {
		return existing
	}

	return &models.SubscriptionRule{ID: idFromReq(ruleReq.ID), Rule: ruleReq.Rule, Type: ruleReq.Type, Enable: ruleReq.Enable}
}

func RespFromSubscriptions(subs []*models.Subscription) types.SubscriptionsRes {
	list := make([]types.SubscriptionRes, len(subs))
	for i, sub := range subs {
		list[i] = RespFromSubscription(sub, true)
	}
	return types.SubscriptionsRes{Subscriptions: &list}
}

func RespFromSubscription(sub *models.Subscription, withRules bool) types.SubscriptionRes {
	res := types.SubscriptionRes{
		ID:         sub.ID,
		Name:       sub.Name,
		Interface:  sub.Interface,
		Enable:     sub.Enable,
		URL:        sub.URL,
		Interval:   sub.Interval,
		LastUpdate: sub.LastUpdate,
	}
	if withRules {
		res.SubscriptionRulesRes = RespFromSubscriptionRules(sub.Rules)
	}
	return res
}

func RespFromSubscriptionRules(rules []*models.SubscriptionRule) types.SubscriptionRulesRes {
	list := make([]types.SubscriptionRuleRes, len(rules))
	for i, rule := range rules {
		list[i] = RespFromSubscriptionRule(rule)
	}
	return types.SubscriptionRulesRes{Rules: &list}
}

func RespFromSubscriptionRule(rule *models.SubscriptionRule) types.SubscriptionRuleRes {
	return types.SubscriptionRuleRes{
		ID:     rule.ID,
		Rule:   rule.Rule,
		Type:   rule.Type,
		Enable: rule.Enable,
	}
}
