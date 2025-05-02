package v1

import (
	"fmt"

	"github.com/dlclark/regexp2"

	"magitrickle/api/types"
	"magitrickle/models"
)

var colorRegExp = regexp2.MustCompile(`^#[0-9A-Fa-f]{6}$`, 0)

func FromGroupReq(req types.GroupReq, existing *models.Group) (*models.Group, error) {
	var group *models.Group
	if existing == nil {
		group = &models.Group{ID: types.RandomID()}
	} else {
		group = existing
	}
	if req.ID != nil {
		if existing != nil && group.ID != *req.ID {
			return nil, fmt.Errorf("group ID mismatch")
		}
		if existing == nil {
			group.ID = *req.ID
		}
	}
	group.Name = req.Name
	if match, _ := colorRegExp.MatchString(req.Color); !match {
		req.Color = "#ffffff"
	}
	group.Color = req.Color
	group.Interface = req.Interface
	group.Enable = true
	// TODO: Make required after 1.0.0
	if req.Enable != nil {
		group.Enable = *req.Enable
	}

	if req.Rules != nil {
		newRules := make([]*models.Rule, len(*req.Rules))
		for i, ruleReq := range *req.Rules {
			r, err := FromRuleReq(ruleReq, group.Rules)
			if err != nil {
				return nil, err
			}
			newRules[i] = r
		}
		group.Rules = newRules
	}
	return group, nil
}

// fromRuleReq конвертирует RuleReq в Rule.
func FromRuleReq(ruleReq types.RuleReq, existingRules []*models.Rule) (*models.Rule, error) {
	var rule *models.Rule
	if ruleReq.ID != nil {
		for _, r := range existingRules {
			if r.ID == *ruleReq.ID {
				rule = r
				break
			}
		}
	}
	if rule == nil {
		rule = &models.Rule{
			ID: types.RandomID(),
		}
	}
	rule.Name = ruleReq.Name
	rule.Type = ruleReq.Type
	rule.Rule = ruleReq.Rule
	rule.Enable = ruleReq.Enable
	return rule, nil
}

func ToGroupsRes(groups []*models.Group, withRules bool) types.GroupsRes {
	groupResList := make([]types.GroupRes, len(groups))
	for i, group := range groups {
		groupResList[i] = ToGroupRes(group, withRules)
	}
	return types.GroupsRes{Groups: &groupResList}
}

func ToGroupRes(group *models.Group, withRules bool) types.GroupRes {
	groupRes := types.GroupRes{
		ID:        group.ID,
		Name:      group.Name,
		Color:     group.Color,
		Interface: group.Interface,
		Enable:    group.Enable,
	}
	if withRules {
		groupRes.RulesRes = ToRulesRes(group.Rules)
	}
	return groupRes
}

func ToRulesRes(rules []*models.Rule) types.RulesRes {
	ruleResList := make([]types.RuleRes, len(rules))
	for i, rule := range rules {
		ruleResList[i] = ToRuleRes(rule)
	}
	return types.RulesRes{Rules: &ruleResList}
}

func ToRuleRes(rule *models.Rule) types.RuleRes {
	return types.RuleRes{
		ID:     rule.ID,
		Name:   rule.Name,
		Type:   rule.Type,
		Rule:   rule.Rule,
		Enable: rule.Enable,
	}
}
