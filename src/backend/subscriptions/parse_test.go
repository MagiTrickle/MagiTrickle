package subscriptions

import (
	"slices"
	"testing"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

func TestRefreshRulesPreservesExistingOverrides(t *testing.T) {
	existing := []*models.SubscriptionRule{
		{
			ID:     intID.ID{0xaa, 0xbb, 0xcc, 0xdd},
			Rule:   "example.com",
			Type:   models.RuleTypeDomain,
			Enable: false,
		},
		{
			ID:     intID.ID{0x11, 0x22, 0x33, 0x44},
			Rule:   "*.example.org",
			Type:   models.RuleTypeWildcard,
			Enable: true,
		},
	}

	refreshed := RefreshRules("example.com\nsub.example.net\n", existing)
	if len(refreshed) != 2 {
		t.Fatalf("expected 2 refreshed rules, got %d", len(refreshed))
	}

	if refreshed[0].Rule != "example.com" {
		t.Fatalf("expected first rule to stay aligned with parsed order, got %q", refreshed[0].Rule)
	}
	if refreshed[0].ID != existing[0].ID {
		t.Fatalf("expected existing ID to be preserved, got %s want %s", refreshed[0].ID.String(), existing[0].ID.String())
	}
	if refreshed[0].Type != existing[0].Type {
		t.Fatalf("expected existing type to be preserved, got %q want %q", refreshed[0].Type, existing[0].Type)
	}
	if refreshed[0].Enable != existing[0].Enable {
		t.Fatalf("expected existing enabled flag to be preserved, got %v want %v", refreshed[0].Enable, existing[0].Enable)
	}

	if refreshed[1].Rule != "sub.example.net" {
		t.Fatalf("expected second parsed rule to be kept, got %q", refreshed[1].Rule)
	}
	if refreshed[1].ID.IsZero() {
		t.Fatal("expected new rule to receive an ID")
	}
	if refreshed[1].Enable != true {
		t.Fatalf("expected new rule to default to enabled, got %v", refreshed[1].Enable)
	}
}

func TestSameRulesIgnoresOrder(t *testing.T) {
	left := []*models.SubscriptionRule{
		{ID: intID.ID{0x01, 0x02, 0x03, 0x04}, Rule: "example.com", Type: models.RuleTypeDomain, Enable: true},
		{ID: intID.ID{0x05, 0x06, 0x07, 0x08}, Rule: "*.example.org", Type: models.RuleTypeWildcard, Enable: false},
	}
	right := []*models.SubscriptionRule{
		{ID: intID.ID{0x05, 0x06, 0x07, 0x08}, Rule: "*.example.org", Type: models.RuleTypeWildcard, Enable: false},
		{ID: intID.ID{0x01, 0x02, 0x03, 0x04}, Rule: "example.com", Type: models.RuleTypeDomain, Enable: true},
	}

	if !sameRules(left, right) {
		t.Fatal("expected rule comparison to ignore order for equal content")
	}
}

func TestEnsureUniqueRuleIDsDoesNotModifySharedRules(t *testing.T) {
	shared := &models.SubscriptionRule{ID: intID.ID{1}, Rule: "a.com"}
	rules := []*models.SubscriptionRule{
		shared,
		{ID: intID.ID{1}, Rule: "b.com"},
		{Rule: "c.com"},
	}
	original := slices.Clone(rules)

	fixed, count := EnsureUniqueRuleIDs(rules)
	if count != 2 {
		t.Fatalf("fixed %d rules, want 2", count)
	}
	seen := map[intID.ID]bool{}
	for _, rule := range fixed {
		if rule.ID.IsZero() || seen[rule.ID] {
			t.Fatalf("rule %q has zero or duplicate ID %s", rule.Rule, rule.ID)
		}
		seen[rule.ID] = true
	}
	if !slices.Equal(rules, original) || rules[1].ID != (intID.ID{1}) || !rules[2].ID.IsZero() {
		t.Error("input slice or its rules were modified in place")
	}
	if fixed[0] != shared {
		t.Error("rule with a valid ID was copied needlessly")
	}
}
