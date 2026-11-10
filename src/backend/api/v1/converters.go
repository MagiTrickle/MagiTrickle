package v1

import (
	"fmt"
	"strings"

	"magitrickle/api/v1/types"
	"magitrickle/models"
	"magitrickle/utils/intID"

	"github.com/dlclark/regexp2"
)

var colorRegExp = regexp2.MustCompile(`^#[0-9a-f]{6}$`, regexp2.IgnoreCase)

// GroupFromReq строит группу из запроса. existing — текущая версия группы,
// если она обновляется; она не изменяется, результат — новая группа.
// ID группы и правил берутся из запроса, если клиент их передал, иначе
// генерируются; уникальность проверяет App при сохранении.
func GroupFromReq(req types.GroupReq, existing *models.Group) (*models.Group, error) {
	var group *models.Group
	if existing == nil {
		group = &models.Group{ID: idFromReq(req.ID)}
	} else {
		if req.ID != nil && !req.ID.IsZero() && *req.ID != existing.ID {
			return nil, fmt.Errorf("group ID mismatch")
		}
		group = existing.Clone()
	}
	group.Name = req.Name
	group.Color = "#ffffff"
	if match, _ := colorRegExp.MatchString(req.Color); match {
		group.Color = strings.ToLower(req.Color)
	}
	group.Interface = req.Interface
	group.Enable = true
	if req.Enable != nil {
		group.Enable = *req.Enable
	}

	if req.Rules != nil {
		group.Rules = RulesFromReq(*req.Rules, group.Rules)
	}
	return group, nil
}

// RulesFromReq строит правила из запроса. Правило, которое не изменилось
// (тот же ID и те же поля, что в existing), берётся прежним объектом: по
// указателям на правила кэш наборов правил узнаёт, что их не нужно
// разбирать заново.
func RulesFromReq(req []types.RuleReq, existing []*models.Rule) []*models.Rule {
	byID := make(map[intID.ID]*models.Rule, len(existing))
	for _, rule := range existing {
		byID[rule.ID] = rule
	}
	rules := make([]*models.Rule, len(req))
	for i, ruleReq := range req {
		rule := RuleFromReq(ruleReq)
		if old := byID[rule.ID]; old != nil && sameRule(old, rule) {
			rule = old
		}
		rules[i] = rule
	}
	return rules
}

func sameRule(a, b *models.Rule) bool {
	return a.ID == b.ID && a.Name == b.Name && a.Type == b.Type && a.Rule == b.Rule && a.Enable == b.Enable
}

func RuleFromReq(req types.RuleReq) *models.Rule {
	return &models.Rule{
		ID:     idFromReq(req.ID),
		Name:   req.Name,
		Type:   req.Type,
		Rule:   req.Rule,
		Enable: req.Enable,
	}
}

// idFromReq возвращает ID из запроса или новый, если ID не передан. Нулевой
// ID считается непереданным.
func idFromReq(id *intID.ID) intID.ID {
	if id != nil && !id.IsZero() {
		return *id
	}
	return intID.RandomID()
}

func RespFromGroups(groups []*models.Group, withRules bool) types.GroupsRes {
	groupResList := make([]types.GroupRes, len(groups))
	for i, group := range groups {
		groupResList[i] = RespFromGroup(group, withRules)
	}
	return types.GroupsRes{Groups: &groupResList}
}

func RespFromGroup(group *models.Group, withRules bool) types.GroupRes {
	groupRes := types.GroupRes{
		ID:        group.ID,
		Name:      group.Name,
		Color:     group.Color,
		Interface: group.Interface,
		Enable:    group.Enable,
	}
	if withRules {
		groupRes.RulesRes = RespFromRules(group.Rules)
	}
	return groupRes
}

func RespFromRules(rules []*models.Rule) types.RulesRes {
	ruleResList := make([]types.RuleRes, len(rules))
	for i, rule := range rules {
		ruleResList[i] = RespFromRule(rule)
	}
	return types.RulesRes{Rules: &ruleResList}
}

func RespFromRule(rule *models.Rule) types.RuleRes {
	return types.RuleRes{
		ID:     rule.ID,
		Name:   rule.Name,
		Type:   rule.Type,
		Rule:   rule.Rule,
		Enable: rule.Enable,
	}
}
