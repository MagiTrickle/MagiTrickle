package groups

import (
	"magitrickle/models"
	"magitrickle/rulesets"
)

func BuildRuntimeRuleSets(groups []*models.Group) []rulesets.Spec {
	specs := make([]rulesets.Spec, 0, len(groups))
	for _, group := range groups {
		if group == nil {
			continue
		}

		specs = append(specs, BuildRuntimeRuleSet(group))
	}
	return specs
}

func BuildRuntimeRuleSet(group *models.Group) rulesets.Spec {
	return rulesets.Spec{
		Model:      group,
		ID:         group.ID,
		RuntimeKey: group.ID.String(),
		Name:       group.Name,
		Interface:  group.Interface,
		Enable:     group.Enable,
		Rules:      group.Rules,
	}
}
