package subscriptions

import (
	"testing"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

func TestBuildRuntimeRuleSetsUsesSubscriptionIDAndNamespacedRuntimeKey(t *testing.T) {
	subID := intID.ID{0xaa, 0xbb, 0xcc, 0xdd}
	ruleID := intID.ID{0x01, 0x02, 0x03, 0x04}

	groups := BuildRuntimeRuleSets([]*models.Subscription{
		{
			ID:        subID,
			Name:      "Example",
			Interface: "nwg0",
			Enable:    true,
			Rules: []*models.SubscriptionRule{
				{
					ID:     ruleID,
					Rule:   "example.com",
					Type:   models.RuleTypeDomain,
					Enable: true,
				},
			},
		},
	})

	if len(groups) != 1 {
		t.Fatalf("expected 1 runtime rule set, got %d", len(groups))
	}

	group := groups[0]
	if group.ID != subID {
		t.Fatalf("expected runtime rule set ID %s, got %s", subID.String(), group.ID.String())
	}
	if want := runtimeKeyPrefix + subID.String(); group.RuntimeKey != want {
		t.Fatalf("expected runtime key %q, got %q", want, group.RuntimeKey)
	}
	if len(group.Rules) != 1 {
		t.Fatalf("expected 1 runtime rule, got %d", len(group.Rules))
	}
	if group.Rules[0].ID != ruleID {
		t.Fatalf("expected runtime rule ID %s, got %s", ruleID.String(), group.Rules[0].ID.String())
	}
}

func TestBuildRuntimeRuleSetsDisablesRuleSetWithoutInterface(t *testing.T) {
	subID := intID.ID{0x10, 0x20, 0x30, 0x40}

	groups := BuildRuntimeRuleSets([]*models.Subscription{
		{
			ID:     subID,
			Enable: true,
		},
	})

	if len(groups) != 1 {
		t.Fatalf("expected 1 runtime rule set, got %d", len(groups))
	}
	if groups[0].Enable {
		t.Fatal("expected runtime rule set without interface to be disabled")
	}
}
