package dnsMITMProxy

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"

	"github.com/miekg/dns"
	"github.com/rs/zerolog/log"
	"golang.org/x/net/ipv4"
	"golang.org/x/net/ipv6"
)

const (
	maxTCPMsgSize        = 65535
	defaultMaxIdleConns  = 10
	defaultMaxConcurrent = 100
	defaultTimeout       = 5 * time.Second
	acceptTimeout        = 1 * time.Second
)

type DNSMITMProxy struct {
	RequestHook  func(net.Addr, dns.Msg, string) (*dns.Msg, *dns.Msg, error)
	ResponseHook func(net.Addr, dns.Msg, dns.Msg, string) (*dns.Msg, error)

	// Private fields
	bufferPool  *sync.Pool
	tcpConnPool *connPool
	udpConnPool *connPool
	semaphore   chan struct{}
}

func NewDNSMITMProxy(addr string) *DNSMITMProxy {
	return &DNSMITMProxy{
		bufferPool: &sync.Pool{
			New: func() interface{} {
				buf := make([]byte, dns.MaxMsgSize)
				return &buf
			},
		},
		tcpConnPool: newConnPool("tcp", addr, defaultMaxIdleConns),
		udpConnPool: newConnPool("udp", addr, defaultMaxIdleConns),
		semaphore:   make(chan struct{}, defaultMaxConcurrent),
	}
}

// Close closes all connection pools and releases resources
func (p *DNSMITMProxy) Close() error {
	if p.tcpConnPool != nil {
		p.tcpConnPool.Close()
	}
	if p.udpConnPool != nil {
		p.udpConnPool.Close()
	}
	return nil
}

func (p *DNSMITMProxy) requestUpstreamDNS(ctx context.Context, req []byte, network string) ([]byte, error) {
	var pool *connPool
	if network == "tcp" {
		pool = p.tcpConnPool
	} else {
		pool = p.udpConnPool
	}

	upstreamConn, err := pool.Get(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to dial DNS upstream: %w", err)
	}

	// Set deadline based on context or default timeout
	deadline, ok := ctx.Deadline()
	if !ok {
		deadline = time.Now().Add(defaultTimeout)
	}
	err = upstreamConn.SetDeadline(deadline)
	if err != nil {
		_ = upstreamConn.Close()
		return nil, fmt.Errorf("failed to set deadline: %w", err)
	}

	if network == "tcp" {
		lenBuf := []byte{byte(len(req) >> 8), byte(len(req))}
		_, err = upstreamConn.Write(lenBuf)
		if err != nil {
			_ = upstreamConn.Close()
			return nil, fmt.Errorf("failed to write length: %w", err)
		}
	}

	_, err = upstreamConn.Write(req)
	if err != nil {
		_ = upstreamConn.Close()
		return nil, fmt.Errorf("failed to write request: %w", err)
	}

	var resp []byte
	if network == "tcp" {
		// Read length prefix directly with bytes
		lenBuf := make([]byte, 2)
		_, err = io.ReadFull(upstreamConn, lenBuf)
		if err != nil {
			_ = upstreamConn.Close()
			return nil, fmt.Errorf("failed to read length: %w", err)
		}
		respLen := int(lenBuf[0])<<8 | int(lenBuf[1])
		if respLen > maxTCPMsgSize {
			_ = upstreamConn.Close()
			return nil, fmt.Errorf("response too large: %d", respLen)
		}

		resp = make([]byte, respLen)
		_, err = io.ReadFull(upstreamConn, resp)
		if err != nil {
			_ = upstreamConn.Close()
			return nil, fmt.Errorf("failed to read response: %w", err)
		}
	} else {
		bufPtr := p.bufferPool.Get().(*[]byte)
		defer p.bufferPool.Put(bufPtr)
		buf := *bufPtr

		n, err := upstreamConn.Read(buf)
		if err != nil {
			_ = upstreamConn.Close()
			return nil, fmt.Errorf("failed to read response: %w", err)
		}
		resp = make([]byte, n)
		copy(resp, buf[:n])
	}

	// Return connection to pool
	pool.Put(upstreamConn)

	return resp, nil
}

func (p *DNSMITMProxy) processReq(ctx context.Context, clientAddr net.Addr, req []byte, network string) ([]byte, error) {
	// Check context before processing
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}

	var reqMsg dns.Msg
	if p.RequestHook != nil || p.ResponseHook != nil {
		err := reqMsg.Unpack(req)
		if err != nil {
			return nil, fmt.Errorf("failed to parse request: %w", err)
		}
	}

	if p.RequestHook != nil {
		modifiedReq, modifiedResp, err := p.RequestHook(clientAddr, reqMsg, network)
		if err != nil {
			return nil, fmt.Errorf("request hook error: %w", err)
		}
		if modifiedResp != nil {
			resp, err := modifiedResp.Pack()
			if err != nil {
				return nil, fmt.Errorf("failed to send modified response: %w", err)
			}
			return resp, nil
		}
		if modifiedReq != nil {
			reqMsg = *modifiedReq
			req, err = reqMsg.Pack()
			if err != nil {
				return nil, fmt.Errorf("failed to pack modified request: %w", err)
			}
		}
	}

	// Check context before making upstream request
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}

	resp, err := p.requestUpstreamDNS(ctx, req, network)
	if err != nil {
		return nil, fmt.Errorf("failed to send request: %w", err)
	}

	if p.ResponseHook != nil {
		var respMsg dns.Msg
		err = respMsg.Unpack(resp)
		respMsg.Compress = true
		if err != nil {
			return nil, fmt.Errorf("failed to parse response: %w", err)
		}

		modifiedResp, err := p.ResponseHook(clientAddr, reqMsg, respMsg, network)
		if err != nil {
			return nil, fmt.Errorf("response hook error: %w", err)
		}
		if modifiedResp != nil {
			resp, err = modifiedResp.Pack()
			if err != nil {
				return nil, fmt.Errorf("failed to send modified response: %w", err)
			}
			return resp, nil
		}
	}

	return resp, nil
}

func (p *DNSMITMProxy) handleTCPConnection(ctx context.Context, clientConn net.Conn) {
	defer func() { _ = clientConn.Close() }()

	// Set read timeout for receiving the request (separate from processing timeout)
	_ = clientConn.SetReadDeadline(time.Now().Add(defaultTimeout))

	// Read length prefix directly with bytes
	lenBuf := make([]byte, 2)
	_, err := io.ReadFull(clientConn, lenBuf)
	if err != nil {
		if ctx.Err() == nil {
			var networkErr net.Error
			if errors.As(err, &networkErr) && networkErr.Timeout() {
				log.Debug().Msg("client read timeout")
			} else {
				log.Error().Err(err).Msg("failed to read length")
			}
		}
		return
	}
	reqLen := int(lenBuf[0])<<8 | int(lenBuf[1])
	if reqLen > maxTCPMsgSize {
		log.Error().Int("length", reqLen).Msg("request too large")
		return
	}

	req := make([]byte, reqLen)
	_, err = io.ReadFull(clientConn, req)
	if err != nil {
		if ctx.Err() == nil {
			log.Error().Err(err).Msg("failed to read tcp request")
		}
		return
	}

	// Now that we have the request, create processing timeout context
	reqCtx, cancel := context.WithTimeout(ctx, defaultTimeout)
	defer cancel()

	// Set deadline for processing and response
	if deadline, ok := reqCtx.Deadline(); ok {
		_ = clientConn.SetDeadline(deadline)
	}

	resp, err := p.processReq(reqCtx, clientConn.RemoteAddr(), req, "tcp")
	if err != nil {
		if reqCtx.Err() != nil {
			log.Debug().Msg("request cancelled by context")
			return
		}
		var networkErr net.Error
		if errors.As(err, &networkErr) && networkErr.Timeout() {
			log.Warn().Err(err).Msg("connection deadline exceeded")
		} else {
			log.Error().Err(err).Msg("failed to process request")
		}
		return
	}

	// Write length prefix directly with bytes
	respLenBuf := []byte{byte(len(resp) >> 8), byte(len(resp))}
	_, err = clientConn.Write(respLenBuf)
	if err != nil {
		if reqCtx.Err() == nil {
			log.Error().Err(err).Msg("failed to send length")
		}
		return
	}
	_, err = clientConn.Write(resp)
	if err != nil {
		if reqCtx.Err() == nil {
			log.Error().Err(err).Msg("failed to send response")
		}
		return
	}
}

func (p *DNSMITMProxy) ListenTCP(ctx context.Context, addr *net.TCPAddr) error {
	listener, err := net.ListenTCP("tcp", addr)
	if err != nil {
		return fmt.Errorf("failed to listen tcp port: %v", err)
	}
	defer func() { _ = listener.Close() }()

	// Close listener when context is cancelled
	go func() {
		<-ctx.Done()
		_ = listener.Close()
	}()

	for {
		// Set deadline for periodic context check
		_ = listener.SetDeadline(time.Now().Add(acceptTimeout))

		conn, err := listener.Accept()
		if err != nil {
			// Check if context is done
			if ctx.Err() != nil {
				return nil
			}

			// Check if it's a timeout (expected for deadline)
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Timeout() {
				continue
			}

			log.Error().Err(err).Msg("tcp connection error")
			continue
		}

		// Acquire semaphore
		select {
		case p.semaphore <- struct{}{}:
		case <-ctx.Done():
			_ = conn.Close()
			return nil
		}

		go func() {
			defer func() { <-p.semaphore }()
			p.handleTCPConnection(ctx, conn)
		}()
	}
}

func (p *DNSMITMProxy) handleUDPConnection(ctx context.Context, pconn4 *ipv4.PacketConn, pconn6 *ipv6.PacketConn, requestedAddr net.IP, clientAddr *net.UDPAddr, req []byte, isIPv4 bool, ifIndex int) {
	// Create context with timeout for this request
	reqCtx, cancel := context.WithTimeout(ctx, defaultTimeout)
	defer cancel()

	resp, err := p.processReq(reqCtx, clientAddr, req, "udp")
	if err != nil {
		if reqCtx.Err() != nil {
			log.Debug().Msg("request cancelled by context")
			return
		}
		var networkErr net.Error
		if errors.As(err, &networkErr) && networkErr.Timeout() {
			log.Warn().Err(err).Msg("connection deadline exceeded")
		} else {
			log.Error().Err(err).Msg("failed to process request")
		}
		return
	}

	if isIPv4 {
		outCM := &ipv4.ControlMessage{
			Src:     requestedAddr,
			IfIndex: ifIndex,
		}
		_, err = pconn4.WriteTo(resp, outCM, clientAddr)
	} else {
		outCM := &ipv6.ControlMessage{
			Src:     requestedAddr,
			IfIndex: ifIndex,
		}
		_, err = pconn6.WriteTo(resp, outCM, clientAddr)
	}
	if err != nil {
		log.Error().Err(err).Msg("failed to send response")
		return
	}
}

func (p *DNSMITMProxy) ListenUDP(ctx context.Context, addr *net.UDPAddr) error {
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return fmt.Errorf("failed to listen udp port: %v", err)
	}
	defer func() { _ = conn.Close() }()

	// Determine address type based on listening address
	// IPv4-only: explicit IPv4 address (e.g., 0.0.0.0 or 192.168.1.1)
	// Dual-stack/IPv6: nil, ::, or specific IPv6 address
	useIPv4Only := addr.IP != nil && addr.IP.To4() != nil

	var pconn4 *ipv4.PacketConn
	var pconn6 *ipv6.PacketConn

	if useIPv4Only {
		pconn4 = ipv4.NewPacketConn(conn)
		if err := pconn4.SetControlMessage(ipv4.FlagDst|ipv4.FlagInterface, true); err != nil {
			return fmt.Errorf("failed to enable control message for IPv4: %w", err)
		}
	} else {
		// Dual-stack or IPv6-only: read via IPv6, write via appropriate conn
		pconn6 = ipv6.NewPacketConn(conn)
		if err := pconn6.SetControlMessage(ipv6.FlagDst|ipv6.FlagInterface, true); err != nil {
			return fmt.Errorf("failed to enable control message for IPv6: %w", err)
		}
		// Need pconn4 for writing responses to IPv4 clients in dual-stack mode
		pconn4 = ipv4.NewPacketConn(conn)
	}

	// Close conn when context is cancelled
	go func() {
		<-ctx.Done()
		_ = conn.Close()
	}()

	for {
		// Set deadline for periodic context check
		_ = conn.SetReadDeadline(time.Now().Add(acceptTimeout))

		var n int
		var clientAddr net.Addr
		var requestedAddr net.IP
		var ifIndex int
		var req []byte

		bufPtr := p.bufferPool.Get().(*[]byte)
		buf := *bufPtr

		if useIPv4Only {
			var cm *ipv4.ControlMessage
			n, cm, clientAddr, err = pconn4.ReadFrom(buf)
			if err == nil && cm != nil {
				requestedAddr = cm.Dst
				ifIndex = cm.IfIndex
			}
		} else {
			var cm *ipv6.ControlMessage
			n, cm, clientAddr, err = pconn6.ReadFrom(buf)
			if err == nil && cm != nil {
				requestedAddr = cm.Dst
				ifIndex = cm.IfIndex
			}
		}

		if err != nil {
			p.bufferPool.Put(bufPtr)

			// Check if context is done
			if ctx.Err() != nil {
				return nil
			}

			// Check if it's a timeout (expected for deadline)
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Timeout() {
				continue
			}

			log.Error().Err(err).Msg("failed to read udp request")
			continue
		} else {
			// Copy request data
			req = make([]byte, n)
			copy(req, buf[:n])
			p.bufferPool.Put(bufPtr)
		}

		clientUDPAddr, ok := clientAddr.(*net.UDPAddr)
		if !ok {
			log.Error().Msg("client addr is not a UDPAddr")
			continue
		}

		isIPv4 := clientUDPAddr.IP.To4() != nil

		if requestedAddr == nil {
			log.Error().Msg("no destination IP in control message")
			continue
		}
		if isIPv4 {
			requestedAddr = requestedAddr.To4()
			if requestedAddr == nil {
				log.Error().Msg("failed to convert IPv6 address to IPv4")
				continue
			}
		}

		// Acquire semaphore
		select {
		case p.semaphore <- struct{}{}:
		case <-ctx.Done():
			return nil
		}

		go func() {
			defer func() { <-p.semaphore }()
			p.handleUDPConnection(ctx, pconn4, pconn6, requestedAddr, clientUDPAddr, req, isIPv4, ifIndex)
		}()
	}
}
