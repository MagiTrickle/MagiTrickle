package magitrickle

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"magitrickle/app"
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

	before := app.Subscriptions()[0]

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

	if before.URL != server.URL+"/old" || before.Rules[0].Rule != "old.example.com" {
		t.Fatal("sync modified the previously published subscription in place")
	}

	snapshot := app.Subscriptions()
	if len(snapshot) != 1 {
		t.Fatalf("stored subscriptions = %d, want 1", len(snapshot))
	}
	if snapshot[0].URL != server.URL+"/new" {
		t.Fatalf("stored URL = %q, want %q", snapshot[0].URL, server.URL+"/new")
	}
	if len(snapshot[0].Rules) != 1 || snapshot[0].Rules[0].Rule != "new.example.com" {
		t.Fatalf("stored rules = %#v, want new.example.com", snapshot[0].Rules)
	}
}

func TestSyncDueSubscriptionsRetriesFailedApplyAfterDelay(t *testing.T) {
	fetches := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fetches++
		_, _ = w.Write([]byte("new.example.com\n"))
	}))
	defer server.Close()

	router := &fakeRouter{broken: map[string]bool{}}
	a := newRunningApp(t, router)
	sub := &models.Subscription{
		ID:        intID.ID{1, 2, 3, 4},
		Enable:    true,
		Interface: "wg0",
		URL:       server.URL,
		Interval:  24 * 3600,
		Rules: []*models.SubscriptionRule{
			{ID: intID.ID{5}, Rule: "old.example.com", Type: models.RuleTypeDomain, Enable: true},
		},
	}
	if err := a.AddSubscription(sub); err != nil {
		t.Fatal(err)
	}

	router.broken["wg0"] = true
	start := time.Unix(1800000000, 0)
	if _, err := a.SyncDueSubscriptions(start); err == nil {
		t.Fatal("SyncDueSubscriptions error = nil, want apply failure")
	}
	stored := a.Subscriptions()[0]
	if stored.Rules[0].Rule != "old.example.com" || stored.LastCheck != 0 {
		t.Fatalf("failed update was not rolled back: rule %q, LastCheck %d", stored.Rules[0].Rule, stored.LastCheck)
	}

	// Через минуту — рано: не скачиваем заново.
	if _, err := a.SyncDueSubscriptions(start.Add(time.Minute)); err != nil {
		t.Fatal(err)
	}
	if fetches != 1 {
		t.Fatalf("subscription fetched %d times within the retry delay, want 1", fetches)
	}

	// Через задержку повтора (а не через сутки интервала) — пробуем снова.
	router.broken["wg0"] = false
	changed, err := a.SyncDueSubscriptions(start.Add(subscriptionRetryDelay))
	if err != nil || !changed {
		t.Fatalf("retry after the delay: changed %v, err %v", changed, err)
	}
	if got := a.Subscriptions()[0].Rules[0].Rule; got != "new.example.com" {
		t.Errorf("rules after retry = %q, want new.example.com", got)
	}
}

func newListServer(t *testing.T, fetches *int) *httptest.Server {
	t.Helper()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		*fetches++
		_, _ = w.Write([]byte("new.example.com\n"))
	}))
	t.Cleanup(server.Close)
	return server
}

func dueSubscription(id byte, iface, url string) *models.Subscription {
	return &models.Subscription{
		ID: intID.ID{id}, Enable: true, Interface: iface, URL: url, Interval: 3600,
		Rules: []*models.SubscriptionRule{{ID: intID.ID{id, 1}, Rule: "old.example.com", Type: models.RuleTypeDomain, Enable: true}},
	}
}

func TestSyncDueSubscriptionsAppliesSubscriptionsIndependently(t *testing.T) {
	fetches := 0
	server := newListServer(t, &fetches)
	router := &fakeRouter{broken: map[string]bool{}}
	a := newRunningApp(t, router)
	if err := a.ReplaceSubscriptions([]*models.Subscription{
		dueSubscription(1, "wg-broken", server.URL),
		dueSubscription(2, "wg0", server.URL),
	}); err != nil {
		t.Fatal(err)
	}

	router.broken["wg-broken"] = true
	changed, err := a.SyncDueSubscriptions(time.Unix(1800000000, 0))
	if err == nil {
		t.Fatal("error = nil, want failure of the broken subscription")
	}
	if !changed {
		t.Error("changed = false although the healthy subscription was updated")
	}
	subs := a.Subscriptions()
	if subs[0].Rules[0].Rule != "old.example.com" {
		t.Error("broken subscription was not rolled back")
	}
	if subs[1].Rules[0].Rule != "new.example.com" {
		t.Error("healthy subscription was rolled back together with the broken one")
	}
}

func TestReplacingSubscriptionDropsPendingRetry(t *testing.T) {
	fetches := 0
	server := newListServer(t, &fetches)
	router := &fakeRouter{broken: map[string]bool{}}
	a := newRunningApp(t, router)
	if err := a.AddSubscription(dueSubscription(1, "wg0", server.URL)); err != nil {
		t.Fatal(err)
	}

	router.broken["wg0"] = true
	start := time.Unix(1800000000, 0)
	_, _ = a.SyncDueSubscriptions(start)

	// Пользователь заменил подписку (например, починил интерфейс).
	router.broken["wg0"] = false
	if err := a.ReplaceSubscriptions([]*models.Subscription{dueSubscription(1, "wg0", server.URL)}); err != nil {
		t.Fatal(err)
	}
	before := fetches
	if _, err := a.SyncDueSubscriptions(start.Add(time.Minute)); err != nil {
		t.Fatal(err)
	}
	if fetches == before {
		t.Error("replaced subscription was not updated because of a retry delay left from the old one")
	}
}

func TestSlowFetchDoesNotBlockEditsAndIsDroppedIfStale(t *testing.T) {
	fetching, release := make(chan struct{}), make(chan struct{})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		close(fetching)
		<-release
		_, _ = w.Write([]byte("new.example.com\n"))
	}))
	t.Cleanup(server.Close)

	router := &fakeRouter{broken: map[string]bool{}}
	a := newRunningApp(t, router)
	if err := a.AddSubscription(dueSubscription(1, "wg0", server.URL)); err != nil {
		t.Fatal(err)
	}

	done := make(chan error, 1)
	go func() {
		_, _, err := a.SyncSubscriptionByID(intID.ID{1}, time.Unix(1800000000, 0), "")
		done <- err
	}()
	<-fetching

	// Пока источник отвечает, пользователь меняет подписку — и не ждёт.
	edited := dueSubscription(1, "wg1", server.URL)
	if err := a.ReplaceSubscriptions([]*models.Subscription{edited}); err != nil {
		t.Fatal(err)
	}
	close(release)

	if err := <-done; !errors.Is(err, app.ErrSubscriptionChanged) {
		t.Fatalf("sync of a subscription edited during fetch = %v, want ErrSubscriptionChanged", err)
	}
	if got := a.Subscriptions()[0]; got != edited {
		t.Error("stale fetch overwrote the edited subscription")
	}
}

func TestFetchSurvivesEditThatKeepsURLAndRules(t *testing.T) {
	fetching, release := make(chan struct{}), make(chan struct{})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		close(fetching)
		<-release
		_, _ = w.Write([]byte("new.example.com\n"))
	}))
	t.Cleanup(server.Close)

	router := &fakeRouter{broken: map[string]bool{}}
	a := newRunningApp(t, router)
	if err := a.AddSubscription(dueSubscription(1, "wg0", server.URL)); err != nil {
		t.Fatal(err)
	}

	done := make(chan error, 1)
	go func() {
		_, _, err := a.SyncSubscriptionByID(intID.ID{1}, time.Unix(1800000000, 0), "")
		done <- err
	}()
	<-fetching

	// Сохранение из UI пересоздаёт подписку, но URL и правила те же.
	renamed := a.Subscriptions()[0].Clone()
	renamed.Name = "renamed"
	if err := a.ReplaceSubscriptions([]*models.Subscription{renamed}); err != nil {
		t.Fatal(err)
	}
	close(release)

	if err := <-done; err != nil {
		t.Fatalf("sync after an unrelated edit = %v", err)
	}
	got := a.Subscriptions()[0]
	if got.Name != "renamed" || got.Rules[0].Rule != "new.example.com" {
		t.Errorf("subscription = %q with %q, want the edit and the fetched rules", got.Name, got.Rules[0].Rule)
	}
}
