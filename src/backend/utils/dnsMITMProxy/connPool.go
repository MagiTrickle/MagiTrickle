package dnsMITMProxy

import (
	"context"
	"errors"
	"net"
	"sync"
)

// connPool is a simple connection pool based on a channel
type connPool struct {
	network string
	addr    string
	pool    chan net.Conn
	maxIdle int
	mu      sync.Mutex
	closed  bool
}

func newConnPool(network, addr string, maxIdle int) *connPool {
	return &connPool{
		network: network,
		addr:    addr,
		pool:    make(chan net.Conn, maxIdle),
		maxIdle: maxIdle,
	}
}

func (p *connPool) Get(ctx context.Context) (net.Conn, error) {
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return nil, errors.New("pool is closed")
	}
	p.mu.Unlock()

	// Check context before trying to get/create connection
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}

	select {
	case conn := <-p.pool:
		return conn, nil
	default:
		var d net.Dialer
		return d.DialContext(ctx, p.network, p.addr)
	}
}

func (p *connPool) Put(conn net.Conn) {
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		_ = conn.Close()
		return
	}
	p.mu.Unlock()

	select {
	case p.pool <- conn:
	default:
		_ = conn.Close()
	}
}

func (p *connPool) Close() {
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return
	}
	p.closed = true
	p.mu.Unlock()

	close(p.pool)
	for conn := range p.pool {
		_ = conn.Close()
	}
}
