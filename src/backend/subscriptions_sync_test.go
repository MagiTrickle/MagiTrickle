package magitrickle

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"magitrickle/models"
	"magitrickle/utils/intID"
)

func TestSyncSubscriptionByIDUsesOverrideURLAndPersistsURL(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/old":
			_, _ = w.Write([]byte("old.example.com\n"))
		case "/new":
			_, _ = w.Write([]byte("new.example.com\n"))
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()

	subID := intID.ID{0xa1, 0xb2, 0xc3, 0xd4}
	app := &App{
		subscriptions: []*models.Subscription{
			{
				ID:     subID,
				Enable: true,
				URL:    server.URL + "/old",
				Rules: []*models.SubscriptionRule{
					{
						ID:     intID.ID{0x11, 0x22, 0x33, 0x44},
						Rule:   "old.example.com",
						Type:   models.RuleTypeDomain,
						Enable: true,
					},
				},
			},
		},
	}

	updated, changed, err := app.SyncSubscriptionByID(subID, time.Unix(1800000000, 0), server.URL+"/new")
	if err != nil {
		t.Fatalf("SyncSubscriptionByID returned error: %v", err)
	}
	if !changed {
		t.Fatal("SyncSubscriptionByID changed = false, want true")
	}
	if updated.URL != server.URL+"/new" {
		t.Fatalf("updated URL = %q, want %q", updated.URL, server.URL+"/new")
	}
	if len(updated.Rules) != 1 || updated.Rules[0].Rule != "new.example.com" {
		t.Fatalf("updated rules = %#v, want new.example.com", updated.Rules)
	}

	app.WithSubscriptions(func(snapshot []*models.Subscription) {
		if len(snapshot) != 1 {
			t.Fatalf("stored subscriptions = %d, want 1", len(snapshot))
		}
		if snapshot[0].URL != server.URL+"/new" {
			t.Fatalf("stored URL = %q, want %q", snapshot[0].URL, server.URL+"/new")
		}
		if len(snapshot[0].Rules) != 1 || snapshot[0].Rules[0].Rule != "new.example.com" {
			t.Fatalf("stored rules = %#v, want new.example.com", snapshot[0].Rules)
		}
	})
}
