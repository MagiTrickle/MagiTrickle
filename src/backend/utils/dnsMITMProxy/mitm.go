package dnsMITMProxy

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"time"

	"github.com/miekg/dns"
	"github.com/rs/zerolog/log"
)

type DNSMITMProxy struct {
	UpstreamDNSAddress string
	UpstreamDNSPort    uint16

	RequestHook  func(net.Addr, dns.Msg, string) (*dns.Msg, *dns.Msg, error)
	ResponseHook func(net.Addr, dns.Msg, dns.Msg, string) (*dns.Msg, error)
}

func (p DNSMITMProxy) requestDNS(req []byte, network string) ([]byte, error) {
	upstreamConn, err := net.Dial(network, net.JoinHostPort(p.UpstreamDNSAddress, fmt.Sprintf("%d", p.UpstreamDNSPort)))
	if err != nil {
		return nil, fmt.Errorf("failed to dial DNS upstream: %w", err)
	}
	defer func() { _ = upstreamConn.Close() }()

	err = upstreamConn.SetDeadline(time.Now().Add(time.Second * 5))
	if err != nil {
		return nil, fmt.Errorf("failed to set deadline: %w", err)
	}

	if network == "tcp" {
		err = binary.Write(upstreamConn, binary.BigEndian, uint16(len(req)))
		if err != nil {
			return nil, fmt.Errorf("failed to write length: %w", err)
		}
	}

	_, err = upstreamConn.Write(req)
	if err != nil {
		return nil, fmt.Errorf("failed to write request: %w", err)
	}

	var resp []byte
	if network == "tcp" {
		var respLen uint16
		err = binary.Read(upstreamConn, binary.BigEndian, &respLen)
		if err != nil {
			return nil, fmt.Errorf("failed to read length: %w", err)
		}
		resp = make([]byte, respLen)
	} else {
		resp = make([]byte, dns.MaxMsgSize)
	}

	n, err := upstreamConn.Read(resp)
	if err != nil {
		return nil, fmt.Errorf("failed to read response: %w", err)
	}

	return resp[:n], nil
}

func (p DNSMITMProxy) processReq(clientAddr net.Addr, req []byte, network string) ([]byte, error) {
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

	resp, err := p.requestDNS(req, network)
	if err != nil {
		return nil, fmt.Errorf("failed to send request: %w", err)
	}

	if p.ResponseHook != nil {
		var respMsg dns.Msg
		err = respMsg.Unpack(resp)
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

func (p DNSMITMProxy) ListenTCP(ctx context.Context, addr *net.TCPAddr) error {
	listener, err := net.ListenTCP("tcp", addr)
	if err != nil {
		return fmt.Errorf("failed to listen tcp port: %v", err)
	}
	defer func() { _ = listener.Close() }()

	for {
		// Exit if context is done
		if ctx.Err() != nil {
			return nil
		}

		conn, err := listener.Accept()
		if err != nil {
			log.Error().Err(err).Msg("tcp connection error")
			continue
		}

		go func(clientConn net.Conn) {
			defer func() { _ = clientConn.Close() }()

			var respLen uint16
			err = binary.Read(clientConn, binary.BigEndian, &respLen)
			if err != nil {
				log.Error().Err(err).Msg("failed to read length")
				return
			}

			req := make([]byte, int(respLen))
			_, err = clientConn.Read(req)
			if err != nil {
				log.Error().Err(err).Msg("failed to read tcp request")
				return
			}

			resp, err := p.processReq(clientConn.RemoteAddr(), req, "tcp")
			if err != nil {
				var networkErr net.Error
				if errors.As(err, &networkErr) && networkErr.Timeout() {
					log.Warn().Err(err).Msg("connection deadline exceeded")
				} else {
					log.Error().Err(err).Msg("failed to process request")
				}
				return
			}

			err = binary.Write(clientConn, binary.BigEndian, uint16(len(resp)))
			if err != nil {
				log.Error().Err(err).Msg("failed to send length")
				return
			}
			_, err = clientConn.Write(resp)
			if err != nil {
				log.Error().Err(err).Msg("failed to send response")
				return
			}
		}(conn)
	}
}

func (p DNSMITMProxy) ListenUDP(ctx context.Context, addr *net.UDPAddr) error {
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return fmt.Errorf("failed to listen udp port: %v", err)
	}
	defer func() { _ = conn.Close() }()

	for {
		// Exit if context is done
		if ctx.Err() != nil {
			return nil
		}

		req := make([]byte, dns.MaxMsgSize)
		n, clientAddr, err := conn.ReadFromUDP(req)
		if err != nil {
			log.Error().Err(err).Msg("failed to read udp request")
			continue
		}
		req = req[:n]

		go func(clientConn *net.UDPConn, clientAddr *net.UDPAddr) {
			resp, err := p.processReq(clientAddr, req, "udp")
			if err != nil {
				var networkErr net.Error
				if errors.As(err, &networkErr) && networkErr.Timeout() {
					log.Warn().Err(err).Msg("connection deadline exceeded")
				} else {
					log.Error().Err(err).Msg("failed to process request")
				}
				return
			}

			_, err = clientConn.WriteToUDP(resp, clientAddr)
			if err != nil {
				log.Error().Err(err).Msg("failed to send response")
				return
			}
		}(conn, clientAddr)
	}
}
