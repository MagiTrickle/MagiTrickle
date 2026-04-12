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
