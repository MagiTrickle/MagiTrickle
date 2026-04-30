package subscriptions

import (
	"fmt"

	"magitrickle/models"
	"magitrickle/rulesets"
)

const runtimeKeyPrefix = "sub-"

func BuildRuntimeRuleSets(subs []*models.Subscription) []rulesets.Spec {
	groups := make([]rulesets.Spec, 0, len(subs))
	for _, sub := range subs {
		if sub == nil {
			continue
		}

		groups = append(groups, subscriptionAsRuntimeRuleSet(sub))
	}
	return groups
}

func subscriptionAsRuntimeRuleSet(sub *models.Subscription) rulesets.Spec {
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

	return rulesets.Spec{
		ID:         sub.ID,
		RuntimeKey: runtimeKeyPrefix + sub.ID.String(),
		Name:       name,
		Interface:  sub.Interface,
		Enable:     enable,
		Rules:      rules,
	}
}
