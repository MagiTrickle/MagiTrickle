package subscriptions

import (
	"strings"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

func ParseRules(list string) []*models.SubscriptionRule {
	rules := make([]*models.SubscriptionRule, 0)
	seenRules := make(map[string]struct{})
	usedRuleIDs := make(map[[4]byte]struct{})
	parts := strings.FieldsFunc(list, func(r rune) bool {
		return r == '\n' || r == ',' || r == '\r'
	})

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
