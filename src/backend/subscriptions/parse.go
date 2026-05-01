package subscriptions

import (
	"strings"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

func ParseRules(list string) []*models.SubscriptionRule {
	rules := make([]*models.SubscriptionRule, 0)
	parts := strings.FieldsFunc(list, func(r rune) bool {
		return r == '\n' || r == ',' || r == '\r'
	})
	seenRules := make(map[string]struct{}, len(parts))
	usedRuleIDs := make(map[[4]byte]struct{}, len(parts))

	for _, part := range parts {
		line := strings.TrimSpace(part)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		ruleType := detectSubscriptionRuleType(line)
		key := ruleType + "|" + line
		if _, exists := seenRules[key]; exists {
			continue
		}
		seenRules[key] = struct{}{}

		rules = append(rules, &models.SubscriptionRule{
			ID:     nextUniqueRuleID(usedRuleIDs),
			Rule:   line,
			Type:   ruleType,
			Enable: true,
		})
	}

	return rules
}

func RefreshRules(list string, existing []*models.SubscriptionRule) []*models.SubscriptionRule {
	parsed := ParseRules(list)
	if len(parsed) == 0 {
		return parsed
	}

	existingByRule := make(map[string]*models.SubscriptionRule, len(existing))
	for _, rule := range existing {
		if rule == nil || rule.Rule == "" {
			continue
		}
		if _, exists := existingByRule[rule.Rule]; exists {
			continue
		}
		existingByRule[rule.Rule] = rule
	}

	usedRuleIDs := make(map[[4]byte]struct{}, len(parsed))
	for _, rule := range parsed {
		if current := existingByRule[rule.Rule]; current != nil {
			rule.ID = current.ID
			rule.Enable = current.Enable
			if current.Type != "" {
				rule.Type = current.Type
			}
		}
		if rule.ID.IsZero() {
			rule.ID = nextUniqueRuleID(usedRuleIDs)
			continue
		}
		if _, exists := usedRuleIDs[rule.ID]; exists {
			rule.ID = nextUniqueRuleID(usedRuleIDs)
			continue
		}
		usedRuleIDs[rule.ID] = struct{}{}
	}

	return parsed
}

type subscriptionRuleState struct {
	ID     intID.ID
	Enable bool
	Count  int
}

func sameRulesKey(rule *models.SubscriptionRule) string {
	if rule == nil {
		return ""
	}

	ruleType := rule.Type
	if ruleType == "" {
		ruleType = detectSubscriptionRuleType(rule.Rule)
	}

	return ruleType + "|" + rule.Rule
}

func sameRules(left, right []*models.SubscriptionRule) bool {
	if len(left) != len(right) {
		return false
	}
	if len(left) == 0 {
		return true
	}

	states := make(map[string]subscriptionRuleState, len(left))
	leftNilCount := 0
	for _, rule := range left {
		if rule == nil {
			leftNilCount++
			continue
		}

		key := sameRulesKey(rule)
		state := states[key]
		if state.Count > 0 && (state.ID != rule.ID || state.Enable != rule.Enable) {
			return false
		}
		state.ID = rule.ID
		state.Enable = rule.Enable
		state.Count++
		states[key] = state
	}

	rightNilCount := 0
	for _, rule := range right {
		if rule == nil {
			rightNilCount++
			continue
		}

		key := sameRulesKey(rule)
		state, exists := states[key]
		if !exists || state.Count == 0 {
			return false
		}
		if state.ID != rule.ID || state.Enable != rule.Enable {
			return false
		}
		state.Count--
		if state.Count == 0 {
			delete(states, key)
			continue
		}
		states[key] = state
	}

	return leftNilCount == rightNilCount && len(states) == 0
}

func detectSubscriptionRuleType(pattern string) string {
	p := strings.TrimSpace(pattern)

	if isValidSubnet6(p) {
		return "subnet6"
	}
	if isValidSubnet(p) {
		return "subnet"
	}
	if isValidNamespace(p) {
		return "namespace"
	}
	if isValidDomain(p) {
		return "domain"
	}
	if isValidRegex(p) {
		return "regex"
	}
	if isValidWildcard(p) {
		return "wildcard"
	}

	return ""
}

func nextUniqueRuleID(used map[[4]byte]struct{}) intID.ID {
	for {
		candidate := intID.RandomID()
		if _, exists := used[candidate]; exists {
			continue
		}
		used[candidate] = struct{}{}
		return candidate
	}
}
