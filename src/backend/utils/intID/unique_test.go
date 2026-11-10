package intID

import (
	"slices"
	"testing"
)

type item struct {
	id   ID
	name string
}

func ensure(items []*item) ([]*item, int) {
	return EnsureUnique(items,
		func(i *item) ID { return i.id },
		func(i *item, id ID) *item { c := *i; c.id = id; return &c })
}

func TestEnsureUniqueFixesZeroAndDuplicatesWithoutTouchingInput(t *testing.T) {
	keep := &item{id: ID{1}, name: "a"}
	items := []*item{keep, {id: ID{1}, name: "b"}, {name: "c"}, {id: ID{2}, name: "d"}}
	original := slices.Clone(items)

	result, fixed := ensure(items)
	if fixed != 2 {
		t.Fatalf("fixed = %d, want 2", fixed)
	}
	seen := map[ID]bool{}
	for _, it := range result {
		if it.id.IsZero() || seen[it.id] {
			t.Fatalf("%s has zero or duplicate ID %s", it.name, it.id)
		}
		seen[it.id] = true
	}
	if result[0] != keep || result[3] != items[3] {
		t.Error("items with valid IDs were replaced")
	}
	if !slices.Equal(items, original) || items[1].id != (ID{1}) || !items[2].id.IsZero() {
		t.Error("input was modified")
	}
}

func TestEnsureUniqueReturnsSameSliceWhenNothingToFix(t *testing.T) {
	items := []*item{{id: ID{1}}, {id: ID{2}}}
	result, fixed := ensure(items)
	if fixed != 0 || &result[0] != &items[0] {
		t.Error("slice was copied although all IDs are valid")
	}
}
