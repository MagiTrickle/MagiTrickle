package v1

import (
	"fmt"

	"magitrickle/api/v1/types"
	"magitrickle/models"
	"magitrickle/utils/intID"
)

func SubscriptionFromReq(req types.SubscriptionReq, existing *models.Subscription) (*models.Subscription, error) {
	var sub *models.Subscription
	if existing == nil {
		sub = &models.Subscription{ID: intID.RandomID()}
	} else {
		sub = existing
	}
	if req.ID != nil {
		if existing != nil && sub.ID != *req.ID {
			return nil, fmt.Errorf("subscription ID mismatch")
		}
		if existing == nil {
			sub.ID = *req.ID
		}
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
	if existing != nil {
		sub.GroupID = existing.GroupID
	}

	if req.Rules != nil {
		newRules := make([]*models.SubscriptionRule, len(*req.Rules))
		for i, ruleReq := range *req.Rules {
			r, err := SubscriptionRuleFromReq(ruleReq, sub.Rules)
			if err != nil {
				return nil, err
			}
			newRules[i] = r
		}
		sub.Rules = newRules
	}

	return sub, nil
}

func SubscriptionRuleFromReq(ruleReq types.SubscriptionRuleReq, existingRules []*models.SubscriptionRule) (*models.SubscriptionRule, error) {
	var rule *models.SubscriptionRule
	if ruleReq.ID != nil {
		for _, r := range existingRules {
			if r.ID == *ruleReq.ID {
				rule = r
				break
			}
		}
	}
	if rule == nil {
		rule = &models.SubscriptionRule{
			ID: intID.RandomID(),
		}
	}
	rule.Rule = ruleReq.Rule
	rule.Type = ruleReq.Type
	rule.Enable = ruleReq.Enable
	return rule, nil
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
