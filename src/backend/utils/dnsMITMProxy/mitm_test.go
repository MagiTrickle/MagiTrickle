package dnsMITMProxy

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/miekg/dns"
)

func freeTCPAddr(t *testing.T) *net.TCPAddr {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr)
}

// startUpstream runs a DNS server over TCP that answers every query with 192.0.2.1.
func startUpstream(t *testing.T) *net.TCPAddr {
	t.Helper()
	addr := freeTCPAddr(t)
	started := make(chan struct{})
	srv := &dns.Server{Addr: addr.String(), Net: "tcp", NotifyStartedFunc: func() { close(started) },
		Handler: dns.HandlerFunc(func(w dns.ResponseWriter, r *dns.Msg) {
			m := new(dns.Msg)
			m.SetReply(r)
			m.Answer = []dns.RR{&dns.A{Hdr: dns.RR_Header{Name: r.Question[0].Name, Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 60}, A: net.ParseIP("192.0.2.1")}}
			_ = w.WriteMsg(m)
		})}
	go func() { _ = srv.ListenAndServe() }()
	<-started
	t.Cleanup(func() { _ = srv.Shutdown() })
	return addr
}

func startProxy(t *testing.T, upstream *net.TCPAddr, maxConcurrent uint, timeout time.Duration) *net.TCPAddr {
	t.Helper()
	p := NewDNSMITMProxy(upstream.String(), 2, maxConcurrent, timeout)
	t.Cleanup(func() { _ = p.Close() })
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	addr := freeTCPAddr(t)
	go func() { _ = p.ListenTCP(ctx, addr) }()
	for i := 0; i < 50; i++ {
		if c, err := net.Dial("tcp", addr.String()); err == nil {
			_ = c.Close()
			return addr
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("proxy did not start")
	return nil
}

func exchangeOn(t *testing.T, conn *dns.Conn, name string) {
	t.Helper()
	m := new(dns.Msg)
	m.SetQuestion(name, dns.TypeA)
	_ = conn.SetDeadline(time.Now().Add(time.Second))
	if err := conn.WriteMsg(m); err != nil {
		t.Fatalf("write: %v", err)
	}
	r, err := conn.ReadMsg()
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if len(r.Answer) != 1 {
		t.Fatalf("answer: %v", r.Answer)
	}
}

// A connection that waits for its next query must not occupy a request slot: with a single slot, a second client
// still gets served while the first one sits idle on an open connection.
func TestIdleTCPConnectionDoesNotHoldARequestSlot(t *testing.T) {
	proxy := startProxy(t, startUpstream(t), 1, 3*time.Second)

	idle, err := dns.DialTimeout("tcp", proxy.String(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer idle.Close()
	exchangeOn(t, idle, "first.example.") // the connection stays open and idle afterwards

	other, err := dns.DialTimeout("tcp", proxy.String(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer other.Close()
	exchangeOn(t, other, "second.example.")

	// The idle connection is still usable: several queries per connection are served.
	exchangeOn(t, idle, "third.example.")
}
