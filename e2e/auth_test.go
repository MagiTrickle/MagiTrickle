package e2e

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
)

func authedConfig() string {
	return strings.Replace(DaemonConfig{ShowAll: true}.YAML(),
		"    auth:\n      enabled: false\n", "    auth:\n      enabled: true\n", 1)
}

func rawDo(t testing.TB, method, path, token string, body any) (int, []byte) {
	t.Helper()
	var r io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		r = bytes.NewReader(b)
	}
	req, _ := http.NewRequest(method, mtBase+path, r)
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, data
}

// With auth enabled the API is protected by a bearer token obtained with the Entware system (root) credentials.
func TestHTTPAuth(t *testing.T) {
	StartDaemonRaw(t, authedConfig())

	// The auth endpoints stay open.
	code, body := rawDo(t, "GET", "/auth", "", nil)
	if code != 200 || !strings.Contains(string(body), `"enabled":true`) {
		t.Fatalf("GET /auth = %d %s", code, body)
	}
	if code, _ := rawDo(t, "GET", "/groups", "", nil); code != 401 {
		t.Errorf("no token: %d", code)
	}
	if code, _ := rawDo(t, "GET", "/groups", "garbage", nil); code != 401 {
		t.Errorf("garbage token: %d", code)
	}
	for name, creds := range map[string]map[string]string{
		"wrong password": {"login": "root", "password": "nope"},
		"unknown user":   {"login": "ghost", "password": sshPass},
	} {
		if code, _ := rawDo(t, "POST", "/auth", "", creds); code != 403 {
			t.Errorf("%s: %d", name, code)
		}
	}
	if code, _ := rawDo(t, "POST", "/auth", "", map[string]string{"login": "root"}); code != 400 {
		t.Errorf("missing password: %d", code)
	}

	code, body = rawDo(t, "POST", "/auth", "", map[string]string{"login": sshUser, "password": sshPass})
	if code != 200 {
		t.Fatalf("login = %d %s", code, body)
	}
	var tok struct{ Token string }
	if err := json.Unmarshal(body, &tok); err != nil || tok.Token == "" {
		t.Fatalf("token: %s", body)
	}
	if code, _ := rawDo(t, "GET", "/groups", tok.Token, nil); code != 200 {
		t.Errorf("valid token: %d", code)
	}
	if code, _ := rawDo(t, "POST", "/groups", tok.Token, Group{Name: "authed", Interface: "eth3"}); code != 200 {
		t.Errorf("valid token POST: %d", code)
	}
	// Tampered signature is rejected.
	if code, _ := rawDo(t, "GET", "/groups", tok.Token+"x", nil); code != 401 {
		t.Errorf("tampered token: %d", code)
	}

	// The UNIX socket used by the ndm hook is not subject to authentication.
	out := rt.MustRun(t, `printf 'GET /api/v1/groups HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | socat - UNIX-CONNECT:`+sockFile)
	if !strings.HasPrefix(out, "HTTP/1.1 200") {
		t.Errorf("unix socket with auth enabled: %q", out)
	}
}

func TestAuthDisabledRejectsLogin(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	code, body := rawDo(t, "GET", "/auth", "", nil)
	if code != 200 || !strings.Contains(string(body), `"enabled":false`) {
		t.Errorf("GET /auth = %d %s", code, body)
	}
	if code, _ := rawDo(t, "POST", "/auth", "", map[string]string{"login": sshUser, "password": sshPass}); code != 404 {
		t.Errorf("login with auth disabled: %d", code)
	}
}

func TestStaticFilesCannotEscapeSkinDir(t *testing.T) {
	StartDaemon(t, DaemonConfig{ShowAll: true})
	for _, p := range []string{"/../../../../etc/passwd", "/%2e%2e/%2e%2e/%2e%2e/etc/passwd", "/..%2f..%2f..%2fetc/passwd"} {
		req, _ := http.NewRequest("GET", "http://127.0.0.1:8081"+p, nil)
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		data, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if strings.Contains(string(data), "root:") {
			t.Errorf("GET %s leaked /etc/passwd", p)
		}
	}
}
