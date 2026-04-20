package subscriptions

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestFetchListFollowsRedirects(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/start":
			http.Redirect(w, r, "/next", http.StatusMovedPermanently)
		case "/next":
			http.Redirect(w, r, "/list", http.StatusFound)
		case "/list":
			_, _ = w.Write([]byte("example.com\n"))
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()

	list, err := FetchList(server.URL + "/start")
	if err != nil {
		t.Fatalf("FetchList returned error: %v", err)
	}
	if list != "example.com\n" {
		t.Fatalf("FetchList returned %q, want %q", list, "example.com\n")
	}
}

func TestFetchListRejectsTooManyRedirects(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path := strings.TrimPrefix(r.URL.Path, "/")
		if path == "" {
			path = "0"
		}
		if path == "6" {
			_, _ = w.Write([]byte("example.com\n"))
			return
		}
		next := byte(path[0]) + 1
		http.Redirect(w, r, "/"+string(next), http.StatusFound)
	}))
	defer server.Close()

	_, err := FetchList(server.URL + "/0")
	if err == nil || !strings.Contains(err.Error(), "too many redirects") {
		t.Fatalf("FetchList error = %v, want too many redirects", err)
	}
}

func TestFetchListRejectsRedirectLoop(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/one":
			http.Redirect(w, r, "/two", http.StatusFound)
		case "/two":
			http.Redirect(w, r, "/one", http.StatusFound)
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()

	_, err := FetchList(server.URL + "/one")
	if err == nil || !strings.Contains(err.Error(), "redirect loop detected") {
		t.Fatalf("FetchList error = %v, want redirect loop detected", err)
	}
}
