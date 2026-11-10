package magitrickle

import (
	"context"
	"sync/atomic"
	"testing"
	"time"

	"github.com/vishvananda/netlink"
)

func TestRetryLoopMergesBurstOfRequests(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	requests := make(chan struct{}, 1)
	var calls atomic.Int32
	done := make(chan struct{})
	go func() {
		defer close(done)
		retryLoop(ctx, requests, time.Hour, 50*time.Millisecond, func() { calls.Add(1) })
	}()

	// Пачка запросов, как при переподключении интерфейса.
	for range 20 {
		select {
		case requests <- struct{}{}:
		default:
		}
		time.Sleep(time.Millisecond)
	}
	time.Sleep(200 * time.Millisecond)
	if got := calls.Load(); got != 1 {
		t.Errorf("burst of requests caused %d retries, want 1", got)
	}

	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("retry loop did not stop after cancel")
	}
}

func TestRetryLoopStopsDuringDebounce(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	requests := make(chan struct{}, 1)
	requests <- struct{}{}
	var calls atomic.Int32
	done := make(chan struct{})
	go func() {
		defer close(done)
		retryLoop(ctx, requests, time.Hour, time.Hour, func() { calls.Add(1) })
	}()
	time.Sleep(10 * time.Millisecond)
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("retry loop did not stop while waiting")
	}
	if calls.Load() != 0 {
		t.Error("retry ran after cancel")
	}
}

func TestInterfaceEventsKeepLatestPerInterface(t *testing.T) {
	events := newInterfaceEvents()
	for i := range 5 {
		events.putAddr("wg0", netlink.AddrUpdate{LinkIndex: i})
	}
	events.putAddr("wg1", netlink.AddrUpdate{LinkIndex: 100})

	select {
	case <-events.ready:
	default:
		t.Fatal("no signal after events were queued")
	}
	links, addrs := events.take()
	if len(links) != 0 || len(addrs) != 2 || addrs["wg0"].LinkIndex != 4 {
		t.Errorf("take() = %v %v, want the latest event of each interface", links, addrs)
	}
	if _, addrs := events.take(); len(addrs) != 0 {
		t.Error("events were not cleared by take")
	}
}
