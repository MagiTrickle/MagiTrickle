package e2e

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"testing"
	"time"
)

const mtBase = "http://127.0.0.1:8081/api/v1"

type Rule struct {
	ID     string `json:"id,omitempty"`
	Name   string `json:"name"`
	Type   string `json:"type"`
	Rule   string `json:"rule"`
	Enable bool   `json:"enable"`
}

type Group struct {
	ID        string  `json:"id,omitempty"`
	Name      string  `json:"name"`
	Color     string  `json:"color"`
	Interface string  `json:"interface"`
	Enable    *bool   `json:"enable,omitempty"`
	Rules     *[]Rule `json:"rules,omitempty"`
}

type SubRule struct {
	ID     string `json:"id,omitempty"`
	Rule   string `json:"rule"`
	Type   string `json:"type"`
	Enable bool   `json:"enable"`
}

type Subscription struct {
	ID         string     `json:"id,omitempty"`
	Name       string     `json:"name"`
	Interface  string     `json:"interface"`
	Enable     *bool      `json:"enable,omitempty"`
	URL        string     `json:"url"`
	Interval   *uint32    `json:"interval,omitempty"`
	LastUpdate *uint32    `json:"lastUpdate,omitempty"`
	Rules      *[]SubRule `json:"rules,omitempty"`
}

func ptr[T any](v T) *T { return &v }

// API is a client for the MagiTrickle HTTP API (host port 8081 is forwarded to the router's :8080).
type API struct{ c *http.Client }

func newAPI() *API { return &API{c: &http.Client{Timeout: 15 * time.Second}} }

// Do returns status code and body.
func (a *API) Do(method, path string, payload any) (int, []byte, error) {
	var body io.Reader
	if payload != nil {
		b, err := json.Marshal(payload)
		if err != nil {
			return 0, nil, err
		}
		body = bytes.NewReader(b)
	}
	req, _ := http.NewRequest(method, mtBase+path, body)
	if payload != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := a.c.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, data, nil
}

// OK performs a request that must return 200 and decodes the response into out (if non-nil).
func (a *API) OK(t testing.TB, method, path string, payload, out any) {
	t.Helper()
	code, data, err := a.Do(method, path, payload)
	if err != nil {
		t.Fatalf("%s %s: %v", method, path, err)
	}
	if code != http.StatusOK {
		t.Fatalf("%s %s: status %d: %s", method, path, code, data)
	}
	if out != nil {
		if err := json.Unmarshal(data, out); err != nil {
			t.Fatalf("%s %s: decode: %v\n%s", method, path, err, data)
		}
	}
}

// Status performs a request and returns only the status code.
func (a *API) Status(t testing.TB, method, path string, payload any) int {
	t.Helper()
	code, _, err := a.Do(method, path, payload)
	if err != nil {
		t.Fatalf("%s %s: %v", method, path, err)
	}
	return code
}

func (a *API) CreateGroup(t testing.TB, g Group) Group {
	t.Helper()
	var out Group
	a.OK(t, "POST", "/groups", g, &out)
	return out
}

func (a *API) Groups(t testing.TB, withRules bool) []Group {
	t.Helper()
	var out struct {
		Groups []Group `json:"groups"`
	}
	path := "/groups"
	if withRules {
		path += "?with_rules=true"
	}
	a.OK(t, "GET", path, nil, &out)
	return out.Groups
}

func (a *API) Group(t testing.TB, id string) Group {
	t.Helper()
	var out Group
	a.OK(t, "GET", "/groups/"+id, nil, &out)
	return out
}

func (a *API) Subscriptions(t testing.TB) []Subscription {
	t.Helper()
	var out struct {
		Subscriptions []Subscription `json:"subscriptions"`
	}
	a.OK(t, "GET", "/subscriptions", nil, &out)
	return out.Subscriptions
}

func (a *API) Save(t testing.TB) { t.Helper(); a.OK(t, "POST", "/system/config/save", nil, nil) }

var _ = fmt.Sprint
