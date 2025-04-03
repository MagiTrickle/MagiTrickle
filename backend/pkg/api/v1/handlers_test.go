package v1

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"

	"github.com/Ponywka/MagiTrickle/backend/pkg/api/types"
	"github.com/Ponywka/MagiTrickle/backend/pkg/app"
)

func setupHTTP(a *app.App, rootRouter chi.Router, errChan chan error) (*http.Server, error) {
	address := fmt.Sprintf("%s:%d",
		a.Config().HTTPWeb.Host.Address,
		a.Config().HTTPWeb.Host.Port,
	)

	listener, err := net.Listen("tcp", address)
	if err != nil {
		return nil, fmt.Errorf("listen error: %v", err)
	}

	srv := &http.Server{Handler: rootRouter}

	go func() {
		if e := srv.Serve(listener); e != nil && e != http.ErrServerClosed {
			errChan <- e
		}
		_ = listener.Close()
	}()

	srv.Addr = listener.Addr().String()

	return srv, nil
}

func TestIntegration(t *testing.T) {
	core := app.New()

	errChan := make(chan error, 1)

	rootRouter := chi.NewRouter()
	rootRouter.Use(middleware.Recoverer)

	h := NewHandler(core)
	apiHandler := NewHandler(core)
	apiRouter := NewRouter(apiHandler)

	apiRouter.Get("/v1/groups", h.GetGroups)
	apiRouter.Post("/v1/groups", h.CreateGroup)

	rootRouter.Mount("/api", apiRouter)

	srv, err := setupHTTP(core, rootRouter, errChan)
	if err != nil {
		t.Fatalf("setupHTTP error: %v", err)
	}

	baseURL := fmt.Sprintf("http://%s/api/v1", srv.Addr)
	t.Logf("Server started at %s", baseURL)

	var once sync.Once
	shutdown := func() {
		once.Do(func() {
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			_ = srv.Shutdown(ctx)
		})
	}
	defer shutdown()

	go func() {
		if e := <-errChan; e != nil {
			t.Logf("server error: %v", e)
			shutdown()
		}
	}()

	//-----------//
	// Тесты API //
	//-----------//

	t.Run("CheckGroupsInitially", func(t *testing.T) {
		resp, body := doRequest(t, http.MethodGet, baseURL+"/groups", nil)
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			responseData, _ := io.ReadAll(body)
			t.Fatalf("GET /groups => %d, want 200. Body: %s", resp.StatusCode, string(responseData))
		}

		var gr types.GroupsRes
		mustDecode(t, body, &gr)

		if gr.Groups == nil {
			t.Log("Groups is nil => нет групп")
		} else {
			t.Logf("We have %d groups", len(*gr.Groups))
		}
	})

	t.Run("CreateGroup", func(t *testing.T) {
		req := types.GroupReq{Name: "TestGroup1"}
		payload, _ := json.Marshal(req)

		resp, body := doRequest(t, http.MethodPost, baseURL+"/groups", payload)
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			responseData, _ := io.ReadAll(body)
			t.Fatalf("POST /groups => %d, want 200. Body: %s", resp.StatusCode, string(responseData))
		}

		var newGrp types.GroupRes
		mustDecode(t, body, &newGrp)

		if newGrp.Name != "TestGroup1" {
			t.Errorf("Expected group name=TestGroup1, got %s", newGrp.Name)
		}
		t.Logf("Created group with ID=%v", newGrp.ID)
	})
}

func doRequest(t *testing.T, method, url string, data []byte) (*http.Response, io.ReadCloser) {
	t.Helper()
	var body io.Reader
	if data != nil {
		body = bytes.NewReader(data)
	}

	req, err := http.NewRequest(method, url, body)
	if err != nil {
		t.Fatalf("http.NewRequest failed: %v", err)
	}
	if data != nil {
		req.Header.Set("Content-Type", "application/json")
	}

	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("%s %s error: %v", method, url, err)
	}

	return resp, resp.Body
}

func mustDecode(t *testing.T, r io.Reader, v interface{}) {
	t.Helper()
	if err := json.NewDecoder(r).Decode(v); err != nil {
		t.Fatalf("JSON decode error: %v", err)
	}
}
