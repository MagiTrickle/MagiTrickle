package e2e

import (
	"bytes"
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/cookiejar"
	"testing"
	"time"
)

const (
	webBase  = "http://127.0.0.1:8080"
	rciLogin = "admin"
	rciPass  = "E2ETests"
	// Entware root account baked into the emulator image.
	sshUser = "root"
	sshPass = "keenetic"
)

// RCI talks to the Keenetic web API (/rci) through the emulator's forwarded web port.
type RCI struct {
	c *http.Client
}

func newRCI() *RCI {
	jar, _ := cookiejar.New(nil)
	return &RCI{c: &http.Client{Jar: jar, Timeout: 20 * time.Second}}
}

func (r *RCI) login() error {
	resp, err := r.c.Get(webBase + "/auth")
	if err != nil {
		return err
	}
	_, _ = io.Copy(io.Discard, resp.Body)
	resp.Body.Close()
	if resp.StatusCode == http.StatusOK {
		return nil
	}
	realm := resp.Header.Get("X-NDM-Realm")
	challenge := resp.Header.Get("X-NDM-Challenge")
	if challenge == "" {
		return fmt.Errorf("no challenge in /auth response (status %d)", resp.StatusCode)
	}
	m := md5.Sum([]byte(rciLogin + ":" + realm + ":" + rciPass))
	s := sha256.Sum256([]byte(challenge + hex.EncodeToString(m[:])))
	body, _ := json.Marshal(map[string]string{"login": rciLogin, "password": hex.EncodeToString(s[:])})
	resp, err = r.c.Post(webBase+"/auth", "application/json", bytes.NewReader(body))
	if err != nil {
		return err
	}
	_, _ = io.Copy(io.Discard, resp.Body)
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("login failed: status %d", resp.StatusCode)
	}
	return nil
}

// Do sends a request, re-authenticating when the session has expired.
func (r *RCI) Do(method, path string, payload any) ([]byte, error) {
	for attempt := 0; attempt < 2; attempt++ {
		var body io.Reader
		if payload != nil {
			b, _ := json.Marshal(payload)
			body = bytes.NewReader(b)
		}
		req, _ := http.NewRequest(method, webBase+path, body)
		if payload != nil {
			req.Header.Set("Content-Type", "application/json")
		}
		resp, err := r.c.Do(req)
		if err != nil {
			return nil, err
		}
		data, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode == http.StatusUnauthorized && attempt == 0 {
			if err := r.login(); err != nil {
				return nil, err
			}
			continue
		}
		if resp.StatusCode != http.StatusOK {
			return data, fmt.Errorf("%s %s: status %d: %s", method, path, resp.StatusCode, data)
		}
		return data, nil
	}
	return nil, fmt.Errorf("unreachable")
}

// Batch posts a list of commands to /rci/ and returns the raw response.
func (r *RCI) Batch(t testing.TB, cmds ...any) []byte {
	t.Helper()
	data, err := r.Do("POST", "/rci/", cmds)
	if err != nil {
		t.Fatalf("rci: %v", err)
	}
	return data
}

// Show runs a GET /rci/show/... query into v.
func (r *RCI) Show(t testing.TB, path string, v any) {
	t.Helper()
	data, err := r.Do("GET", "/rci/show/"+path, nil)
	if err != nil {
		t.Fatalf("rci show %s: %v", path, err)
	}
	if err := json.Unmarshal(data, v); err != nil {
		t.Fatalf("rci show %s: decode: %v\n%s", path, err, data)
	}
}

// SetInterface brings a Keenetic interface up or down.
func (r *RCI) SetInterface(t testing.TB, name string, up bool) {
	t.Helper()
	key := "down"
	if up {
		key = "up"
	}
	r.Batch(t, map[string]any{"interface": map[string]any{"name": name, key: true}})
}

// StaticHost registers a static A record in the ndm DNS proxy.
func (r *RCI) StaticHost(t testing.TB, domain, addr string) {
	t.Helper()
	r.Batch(t, map[string]any{"ip": map[string]any{"host": map[string]any{"domain": domain, "address": addr}}})
}

// CreateWireguard creates a point-to-point Wireguard interface (nwgN in the kernel) with a description.
func (r *RCI) CreateWireguard(t testing.TB, id, description string) {
	t.Helper()
	r.Batch(t,
		map[string]any{"interface": map[string]any{"name": id}},
		map[string]any{"interface": map[string]any{"name": id, "description": description, "up": true}},
	)
}

func (r *RCI) DeleteInterface(t testing.TB, id string) {
	t.Helper()
	r.Batch(t, map[string]any{"no": map[string]any{"interface": map[string]any{"name": id}}})
}
