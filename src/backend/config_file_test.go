package magitrickle

import (
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"magitrickle/constant"
)

func TestWriteFileAtomicReplacesContentAndLeavesNoTempFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config.yaml")
	if err := os.WriteFile(path, []byte("old"), 0o600); err != nil {
		t.Fatal(err)
	}

	if err := writeFileAtomic(path, []byte("new"), 0o600); err != nil {
		t.Fatal(err)
	}

	if got, _ := os.ReadFile(path); string(got) != "new" {
		t.Errorf("content = %q", got)
	}
	assertNoTempFiles(t, path)
	if st, _ := os.Stat(path); st.Mode().Perm() != 0o600 {
		t.Errorf("mode = %v", st.Mode().Perm())
	}
}

// If the new file cannot be put in place, the old one stays untouched and the temporary file is removed.
func TestWriteFileAtomicKeepsTargetWhenRenameFails(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.yaml")
	// A non-empty directory under the target name makes the rename fail.
	if err := os.MkdirAll(filepath.Join(path, "keep"), 0o700); err != nil {
		t.Fatal(err)
	}

	if err := writeFileAtomic(path, []byte("new"), 0o600); err == nil {
		t.Fatal("want an error")
	}

	if _, err := os.Stat(filepath.Join(path, "keep")); err != nil {
		t.Errorf("target was damaged: %v", err)
	}
	assertNoTempFiles(t, path)
}

func assertNoTempFiles(t *testing.T, path string) {
	t.Helper()
	left, _ := filepath.Glob(path + ".*.tmp")
	if len(left) != 0 {
		t.Errorf("temporary files left behind: %v", left)
	}
}

// Saves from the API and from the subscription updater can overlap; none may fail or leave a mix behind.
func TestWriteFileAtomicConcurrentCalls(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config.yaml")
	contents := make(map[string]bool)
	for i := 0; i < 32; i++ {
		contents[strings.Repeat(string(rune('a'+i%26)), 1000+i)] = true
	}

	var wg sync.WaitGroup
	errs := make(chan error, len(contents))
	for c := range contents {
		wg.Add(1)
		go func() {
			defer wg.Done()
			errs <- writeFileAtomic(path, []byte(c), 0o600)
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Errorf("writeFileAtomic: %v", err)
		}
	}

	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !contents[string(got)] {
		t.Errorf("the file holds none of the written contents (%d bytes)", len(got))
	}
	assertNoTempFiles(t, path)
}

// A reload that fails halfway must not leave part of the file applied.
func TestFailedLoadDoesNotChangeTheRunningConfig(t *testing.T) {
	tests := map[string]string{
		"type error after valid keys": "configVersion: 0.1.0\napp:\n  httpWeb:\n    host:\n      port: 9999\n  dnsProxy:\n    maxConcurrent: not-a-number\n",
		"unsupported version":         "configVersion: 9.9.9\napp:\n  httpWeb:\n    host:\n      port: 9999\n",
		"broken yaml":                 "app: [this is: not valid\n  yaml",
	}
	for name, content := range tests {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "config.yaml")
			if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
				t.Fatal(err)
			}
			a := &App{config: constant.DefaultAppConfig}

			if err := a.loadConfigFile(path); err == nil {
				t.Fatal("want an error")
			}

			if a.config.HTTPWeb.Host.Port != constant.DefaultAppConfig.HTTPWeb.Host.Port {
				t.Errorf("port = %d, the failed load changed the running config", a.config.HTTPWeb.Host.Port)
			}
		})
	}
}
