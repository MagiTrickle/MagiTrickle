package groups

import (
	"testing"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

func TestBuildRuntimeRuleSetPreservesModelAndIdentity(t *testing.T) {
	groupID := intID.ID{0xaa, 0xbb, 0xcc, 0xdd}
	model := &models.Group{
		ID:        groupID,
		Name:      "Example",
		Interface: "nwg0",
		Enable:    true,
	}

	spec := BuildRuntimeRuleSet(model)

	if spec.Model != model {
		t.Fatal("expected model pointer to be preserved")
	}
	if spec.ID != groupID {
		t.Fatalf("expected ID %s, got %s", groupID.String(), spec.ID.String())
	}
	if spec.RuntimeKey != groupID.String() {
		t.Fatalf("expected runtime key %q, got %q", groupID.String(), spec.RuntimeKey)
	}
}

func TestBuildRuntimeRuleSetsSkipsNilAndKeepsOrder(t *testing.T) {
	rule := &models.Rule{ID: intID.ID{1, 2, 3, 4}, Type: models.RuleTypeDomain, Rule: "a.test", Enable: true}
	first := &models.Group{ID: intID.ID{0, 0, 0, 1}, Name: "first", Interface: "eth3", Enable: true, Rules: []*models.Rule{rule}}
	second := &models.Group{ID: intID.ID{0, 0, 0, 2}, Name: "second", Interface: "br0"}

	specs := BuildRuntimeRuleSets([]*models.Group{first, nil, second})

	if len(specs) != 2 {
		t.Fatalf("got %d specs, want 2 (nil group skipped)", len(specs))
	}
	if specs[0].Model != first || specs[1].Model != second {
		t.Fatal("order or model pointers not preserved")
	}
	if specs[0].Name != "first" || specs[0].Interface != "eth3" || !specs[0].Enable || specs[1].Enable {
		t.Errorf("fields not copied: %+v %+v", specs[0], specs[1])
	}
	if len(specs[0].Rules) != 1 || specs[0].Rules[0] != rule {
		t.Errorf("rules not shared with the model: %+v", specs[0].Rules)
	}
	if got := BuildRuntimeRuleSets(nil); len(got) != 0 {
		t.Errorf("nil input produced %d specs", len(got))
	}
}
