package e2e

import (
	"bytes"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"golang.org/x/crypto/ssh"
)

// Router is an SSH connection to the Entware shell (dropbear) of the emulated KN-1012.
type Router struct {
	client *ssh.Client
}

func dialRouter(addr string) (*Router, error) {
	cfg := &ssh.ClientConfig{
		User:            sshUser,
		Auth:            []ssh.AuthMethod{ssh.Password(sshPass)},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
		Timeout:         10 * time.Second,
	}
	c, err := ssh.Dial("tcp", addr, cfg)
	if err != nil {
		return nil, err
	}
	return &Router{client: c}, nil
}

// Run executes cmd and returns its combined output.
func (r *Router) Run(cmd string) (string, error) {
	s, err := r.client.NewSession()
	if err != nil {
		return "", err
	}
	defer s.Close()
	out, err := s.CombinedOutput(cmd)
	return string(out), err
}

// MustRun fails the test if cmd exits non-zero.
func (r *Router) MustRun(t testing.TB, cmd string) string {
	t.Helper()
	out, err := r.Run(cmd)
	if err != nil {
		t.Fatalf("router: %q failed: %v\n%s", cmd, err, out)
	}
	return out
}

// Put writes data to a file on the router.
func (r *Router) Put(path string, data []byte, mode os.FileMode) error {
	s, err := r.client.NewSession()
	if err != nil {
		return err
	}
	defer s.Close()
	s.Stdin = bytes.NewReader(data)
	if out, err := s.CombinedOutput(fmt.Sprintf("cat > %s && chmod %o %s", shq(path), mode, shq(path))); err != nil {
		return fmt.Errorf("put %s: %v: %s", path, err, out)
	}
	return nil
}

func (r *Router) MustPut(t testing.TB, path string, data []byte, mode os.FileMode) {
	if err := r.Put(path, data, mode); err != nil {
		if t == nil {
			panic(err)
		}
		t.Helper()
		t.Fatal(err)
	}
}

// Get reads a file from the router.
func (r *Router) Get(path string) (string, error) {
	return r.Run("cat " + shq(path))
}

func shq(s string) string { return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'" }

// LogBuf is a goroutine-safe buffer that tests can poll for log lines.
type LogBuf struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (l *LogBuf) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.buf.Write(p)
}

func (l *LogBuf) String() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.buf.String()
}

// Spawn runs cmd in the background of an SSH session, streaming stdout+stderr to the returned buffer.
// The command is wrapped in `exec` so Process.Signal works through the PID file.
type Process struct {
	sess    *ssh.Session
	Log     *LogBuf
	done    chan struct{}
	exitErr error
	pidFile string
	r       *Router
}

func (r *Router) Spawn(name, cmd string) (*Process, error) {
	s, err := r.client.NewSession()
	if err != nil {
		return nil, err
	}
	p := &Process{sess: s, Log: &LogBuf{}, done: make(chan struct{}), pidFile: "/opt/tmp/" + name + ".pid", r: r}
	s.Stdout = p.Log
	s.Stderr = p.Log
	if err := s.Start(fmt.Sprintf("echo $$ > %s; exec %s", p.pidFile, cmd)); err != nil {
		s.Close()
		return nil, err
	}
	go func() {
		p.exitErr = s.Wait()
		close(p.done)
	}()
	return p, nil
}

// Signal sends a signal (e.g. "TERM") to the process.
func (p *Process) Signal(sig string) {
	_, _ = p.r.Run(fmt.Sprintf("kill -%s $(cat %s) 2>/dev/null", sig, p.pidFile))
}

// Exited reports whether the process has finished.
func (p *Process) Exited() bool {
	select {
	case <-p.done:
		return true
	default:
		return false
	}
}

// Stop sends SIGTERM and waits for exit; falls back to SIGKILL.
func (p *Process) Stop(timeout time.Duration) error {
	p.Signal("TERM")
	select {
	case <-p.done:
	case <-time.After(timeout):
		p.Signal("KILL")
		<-p.done
		return fmt.Errorf("process did not exit within %s after SIGTERM", timeout)
	}
	p.sess.Close()
	return nil
}
