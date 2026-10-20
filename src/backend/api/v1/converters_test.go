package v1

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"

	"magitrickle/app"

	"magitrickle/api/v1/types"
	"magitrickle/models"
	"magitrickle/utils/intID"
)

func TestGroupFromReqKeepsClientIDs(t *testing.T) {
	groupID := intID.ID{1, 2, 3, 4}
	newRuleID := intID.ID{5, 6, 7, 8}
	existing := &models.Group{
		ID:    groupID,
		Rules: []*models.Rule{{ID: intID.ID{9, 9, 9, 9}, Rule: "old.com"}},
	}

	group, err := GroupFromReq(types.GroupReq{
		ID: &groupID,
		RulesReq: types.RulesReq{Rules: &[]types.RuleReq{
			{ID: &newRuleID, Rule: "new.com"},
			{Rule: "no-id.com"},
		}},
	}, existing)
	if err != nil {
		t.Fatal(err)
	}

	if group.ID != groupID {
		t.Errorf("group ID = %s, want %s", group.ID, groupID)
	}
	if group.Rules[0].ID != newRuleID {
		t.Errorf("new rule ID = %s, want client's %s", group.Rules[0].ID, newRuleID)
	}
	if group.Rules[1].ID.IsZero() {
		t.Error("rule without ID got a zero ID")
	}
	if existing.Rules[0].Rule != "old.com" || len(existing.Rules) != 1 {
		t.Error("existing group was modified")
	}
}

func TestGroupFromReqNewGroup(t *testing.T) {
	clientID := intID.ID{1, 2, 3, 4}

	group, err := GroupFromReq(types.GroupReq{ID: &clientID}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if group.ID != clientID {
		t.Errorf("group ID = %s, want client's %s", group.ID, clientID)
	}

	group, err = GroupFromReq(types.GroupReq{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if group.ID.IsZero() {
		t.Error("group without ID got a zero ID")
	}
}

func TestGroupFromReqRejectsIDMismatch(t *testing.T) {
	other := intID.ID{5, 6, 7, 8}

	if _, err := GroupFromReq(types.GroupReq{ID: &other}, &models.Group{ID: intID.ID{1, 2, 3, 4}}); err == nil {
		t.Fatal("GroupFromReq accepted a mismatched ID")
	}
}

func TestZeroIDFromRequestIsTreatedAsAbsent(t *testing.T) {
	var zero intID.ID

	group, err := GroupFromReq(types.GroupReq{
		ID:       &zero,
		RulesReq: types.RulesReq{Rules: &[]types.RuleReq{{ID: &zero, Rule: "a.com"}}},
	}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if group.ID.IsZero() || group.Rules[0].ID.IsZero() {
		t.Errorf("zero ID from the request was kept: group %s, rule %s", group.ID, group.Rules[0].ID)
	}

	existing := &models.Group{ID: intID.ID{1, 2, 3, 4}}
	updated, err := GroupFromReq(types.GroupReq{ID: &zero}, existing)
	if err != nil {
		t.Fatalf("zero ID reported as a mismatch: %v", err)
	}
	if updated.ID != existing.ID {
		t.Errorf("group ID = %s, want %s", updated.ID, existing.ID)
	}
}

func TestRulesFromReqReusesUnchangedRules(t *testing.T) {
	kept := &models.Rule{ID: intID.ID{1}, Name: "a", Type: models.RuleTypeDomain, Rule: "a.com", Enable: true}
	edited := &models.Rule{ID: intID.ID{2}, Name: "b", Type: models.RuleTypeDomain, Rule: "b.com", Enable: true}
	existing := []*models.Rule{kept, edited}

	rules := RulesFromReq([]types.RuleReq{
		{ID: &edited.ID, Name: "b", Type: models.RuleTypeDomain, Rule: "b.org", Enable: true},
		{ID: &kept.ID, Name: "a", Type: models.RuleTypeDomain, Rule: "a.com", Enable: true},
	}, existing)

	if rules[1] != kept {
		t.Error("unchanged rule was recreated")
	}
	if rules[0] == edited || rules[0].Rule != "b.org" || rules[0].ID != edited.ID {
		t.Errorf("edited rule = %+v, want a new object with the same ID", rules[0])
	}
	if edited.Rule != "b.com" {
		t.Error("existing rule was modified in place")
	}
}

func TestSubscriptionRuleFromReqReusesUnchangedRule(t *testing.T) {
	kept := &models.SubscriptionRule{ID: intID.ID{1}, Type: models.RuleTypeDomain, Rule: "a.com", Enable: true}
	existing := map[intID.ID]*models.SubscriptionRule{kept.ID: kept}

	same := SubscriptionRuleFromReq(types.SubscriptionRuleReq{ID: &kept.ID, Type: models.RuleTypeDomain, Rule: "a.com", Enable: true}, existing)
	if same != kept {
		t.Error("unchanged subscription rule was recreated")
	}
	off := SubscriptionRuleFromReq(types.SubscriptionRuleReq{ID: &kept.ID, Type: models.RuleTypeDomain, Rule: "a.com", Enable: false}, existing)
	if off == kept || off.Enable || off.ID != kept.ID || !kept.Enable {
		t.Errorf("changed subscription rule = %+v, want a new disabled copy", off)
	}
}

// saveFailingApp — app.Main, у которого не получается сохранить конфиг.
type saveFailingApp struct{ app.Main }

func (saveFailingApp) SaveConfig() error { return errors.New("disk is read-only") }

func TestSaveFailureIsReportedToClient(t *testing.T) {
	h := NewHandler(saveFailingApp{})

	for _, c := range []struct {
		query         string
		saveByDefault bool
		wantError     bool
	}{
		{"?save=true", false, true},
		{"", false, false},
		{"", true, true},
		{"?save=false", true, false},
	} {
		w := httptest.NewRecorder()
		r := httptest.NewRequest(http.MethodPut, "/groups"+c.query, nil)
		ok := h.save(w, r, c.saveByDefault)
		if ok == c.wantError || (w.Code == http.StatusInternalServerError) != c.wantError {
			t.Errorf("query %q, default %v: ok %v, status %d; want error %v", c.query, c.saveByDefault, ok, w.Code, c.wantError)
		}
	}
}
