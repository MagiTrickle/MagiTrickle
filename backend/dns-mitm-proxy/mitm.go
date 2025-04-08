package dnsMitmProxy

import (
	"context"
	"fmt"
	"net"
	"time"

	"github.com/florianl/go-nfqueue"
	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
	"github.com/mdlayher/netlink"
	"github.com/miekg/dns"
	"github.com/rs/zerolog/log"
)

/*
	TODO: Add to autoinsert
	iptables -t mangle -A PREROUTING -p udp --dport 53 -j NFQUEUE --queue-num 1000 --queue-bypass
	iptables -t mangle -A POSTROUTING -p udp --sport 53 -j NFQUEUE --queue-num 1001 --queue-bypass
*/

var nfqConfigDefault = nfqueue.Config{
	MaxPacketLen: 0xFFFF,
	MaxQueueLen:  0xFF,
	Copymode:     nfqueue.NfQnlCopyPacket,
	WriteTimeout: 50 * time.Millisecond,
}

type ipVersion byte

const (
	ipVersionUnknown ipVersion = iota
	ipVersion4
	ipVersion6
)

type transport byte

const (
	transportUnknown transport = iota
	transportUDP
	transportTCP
)

type direction byte

const (
	directionUnknown direction = iota
	directionInbound
	directionOutbound
)

type DNSMITM struct {
	RequestHook  func(net.IP, net.IP, string, dns.Msg) (*dns.Msg, error)
	ResponseHook func(net.IP, net.IP, string, dns.Msg) (*dns.Msg, error)
}

func (p DNSMITM) processReq(clientAddr net.IP, dnsAddr net.IP, network string, req []byte) ([]byte, error) {
	if p.RequestHook != nil {
		var reqMsg dns.Msg
		err := reqMsg.Unpack(req)
		if err != nil {
			return nil, fmt.Errorf("failed to parse request: %w", err)
		}

		modifiedReq, err := p.RequestHook(clientAddr, dnsAddr, network, reqMsg)
		if err != nil {
			return nil, fmt.Errorf("request hook error: %w", err)
		}

		if modifiedReq != nil {
			reqMsg = *modifiedReq
			req, err = reqMsg.Pack()
			if err != nil {
				return nil, fmt.Errorf("failed to pack modified request: %w", err)
			}
			return req, nil
		}
	}

	return nil, nil
}

func (p DNSMITM) processResp(clientAddr net.IP, dnsAddr net.IP, network string, resp []byte) ([]byte, error) {
	if p.ResponseHook != nil {
		var respMsg dns.Msg
		err := respMsg.Unpack(resp)
		if err != nil {
			return nil, fmt.Errorf("failed to parse response: %w", err)
		}

		modifiedResp, err := p.ResponseHook(clientAddr, dnsAddr, network, respMsg)
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

	return nil, nil
}

func (p DNSMITM) processPacket(nf *nfqueue.Nfqueue, a nfqueue.Attribute, direction direction) int {
	var packet gopacket.Packet
	var ipLayer gopacket.Layer
	ipVersion := func() ipVersion {
		packet = gopacket.NewPacket(*a.Payload, layers.LayerTypeIPv4, gopacket.Default)
		ipLayer = packet.Layer(layers.LayerTypeIPv4)
		if ipLayer != nil {
			return ipVersion4
		}
		packet = gopacket.NewPacket(*a.Payload, layers.LayerTypeIPv6, gopacket.Default)
		ipLayer = packet.Layer(layers.LayerTypeIPv6)
		if ipLayer != nil {
			return ipVersion6
		}
		return 0
	}()

	var transportLayer gopacket.Layer
	transport := func() transport {
		transportLayer = packet.Layer(layers.LayerTypeUDP)
		if transportLayer != nil {
			return transportUDP
		}
		transportLayer = packet.Layer(layers.LayerTypeTCP)
		if transportLayer != nil {
			return transportTCP
		}
		return 0
	}()

	if ipVersion == 0 || transport == 0 {
		log.Error().Msg("failed to detect IP or transport layer")
		nf.SetVerdict(*a.PacketID, nfqueue.NfAccept)
		return 0
	}

	var ipv4 *layers.IPv4
	var ipv6 *layers.IPv6
	var clientAddr net.IP
	var dnsAddr net.IP
	switch ipVersion {
	case ipVersion4:
		ipv4 = ipLayer.(*layers.IPv4)
		switch direction {
		case directionInbound:
			clientAddr = ipv4.SrcIP
			dnsAddr = ipv4.DstIP
		case directionOutbound:
			clientAddr = ipv4.DstIP
			dnsAddr = ipv4.SrcIP
		}
	case ipVersion6:
		ipv6 = ipLayer.(*layers.IPv6)
		switch direction {
		case directionInbound:
			clientAddr = ipv6.SrcIP
			dnsAddr = ipv6.DstIP
		case directionOutbound:
			clientAddr = ipv6.DstIP
			dnsAddr = ipv6.SrcIP
		}
	}

	var udp *layers.UDP
	var tcp *layers.TCP
	switch transport {
	case transportUDP:
		udp = transportLayer.(*layers.UDP)
	case transportTCP:
		tcp = transportLayer.(*layers.TCP)
	}

	var newPayload []byte
	var err error
	switch transport {
	case transportUDP:
		switch direction {
		case directionInbound:
			newPayload, err = p.processReq(clientAddr, dnsAddr, "udp", udp.Payload)
		case directionOutbound:
			newPayload, err = p.processResp(clientAddr, dnsAddr, "udp", udp.Payload)
		}
	case transportTCP:
		switch direction {
		case directionInbound:
			newPayload, err = p.processReq(clientAddr, dnsAddr, "tcp", tcp.Payload)
		case directionOutbound:
			newPayload, err = p.processResp(clientAddr, dnsAddr, "tcp", tcp.Payload)
		}
	}
	if err != nil {
		log.Error().Err(err).Msg("failed to process packet")
		nf.SetVerdict(*a.PacketID, nfqueue.NfAccept)
		return 0
	}
	if newPayload == nil {
		nf.SetVerdict(*a.PacketID, nfqueue.NfAccept)
		return 0
	}

	fmt.Printf("%x\n", newPayload)

	var newIPLayer gopacket.NetworkLayer
	var newIPSerializableLayer gopacket.SerializableLayer
	switch ipVersion {
	case ipVersion4:
		newIPv4 := &layers.IPv4{
			Version:    ipv4.Version,
			IHL:        ipv4.IHL,
			TOS:        ipv4.TOS,
			Id:         ipv4.Id,
			Flags:      ipv4.Flags,
			FragOffset: ipv4.FragOffset,
			TTL:        ipv4.TTL,
			Protocol:   ipv4.Protocol,
			SrcIP:      ipv4.SrcIP,
			DstIP:      ipv4.DstIP,
			//Options:    ipv4.Options,
			//Padding:    ipv4.Padding,
		}
		newIPLayer = newIPv4
		newIPSerializableLayer = newIPv4
	case ipVersion6:
		newIPv6 := &layers.IPv6{
			Version:      ipv6.Version,
			TrafficClass: ipv6.TrafficClass,
			FlowLabel:    ipv6.FlowLabel,
			NextHeader:   ipv6.NextHeader,
			HopLimit:     ipv6.HopLimit,
			SrcIP:        ipv6.SrcIP,
			DstIP:        ipv6.DstIP,
			//HopByHop:     ipv6.HopByHop,
		}
		newIPLayer = newIPv6
		newIPSerializableLayer = newIPv6
	}

	var newTransportSerializableLayer gopacket.SerializableLayer
	switch transport {
	case transportUDP:
		newUDP := &layers.UDP{
			SrcPort: udp.SrcPort,
			DstPort: udp.DstPort,
		}
		newUDP.SetNetworkLayerForChecksum(newIPLayer)
		newTransportSerializableLayer = newUDP
	case transportTCP:
		newTCP := &layers.TCP{
			SrcPort: tcp.SrcPort,
			DstPort: tcp.DstPort,
		}
		newTCP.SetNetworkLayerForChecksum(newIPLayer)
		newTransportSerializableLayer = newTCP
	}

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{
		FixLengths:       true,
		ComputeChecksums: true,
	}
	err = gopacket.SerializeLayers(buf, opts,
		newIPSerializableLayer,
		newTransportSerializableLayer,
		gopacket.Payload(newPayload),
	)
	if err != nil {
		log.Error().Err(err).Msg("failed to serialize packet")
		nf.SetVerdict(*a.PacketID, nfqueue.NfAccept)
		return 0
	}

	// Отправляем модифицированный пакет
	nf.SetVerdictModPacket(*a.PacketID, nfqueue.NfAccept, buf.Bytes())
	return 0
}

func (p DNSMITM) Serve(ctx context.Context) error {
	nfInboundConfig := nfqConfigDefault
	nfInboundConfig.NfQueue = 1000
	nfI, err := nfqueue.Open(&nfInboundConfig)
	if err != nil {
		return fmt.Errorf("nfqueue open: %w", err)
	}
	defer nfI.Close()

	if err := nfI.SetOption(netlink.NoENOBUFS, true); err != nil {
		return fmt.Errorf("failed to set netlink option %v: %w", netlink.NoENOBUFS, err)
	}

	err = nfI.RegisterWithErrorFunc(ctx, func(a nfqueue.Attribute) int {
		return p.processPacket(nfI, a, directionInbound)
	}, func(e error) int {
		fmt.Println(err)
		return -1
	})
	if err != nil {
		return err
	}

	nfOutboundConfig := nfqConfigDefault
	nfOutboundConfig.NfQueue = 1001
	nfO, err := nfqueue.Open(&nfOutboundConfig)
	if err != nil {
		return fmt.Errorf("nfqueue open: %w", err)
	}
	defer nfO.Close()

	if err := nfO.SetOption(netlink.NoENOBUFS, true); err != nil {
		return fmt.Errorf("failed to set netlink option %v: %w", netlink.NoENOBUFS, err)
	}

	err = nfO.RegisterWithErrorFunc(ctx, func(a nfqueue.Attribute) int {
		return p.processPacket(nfO, a, directionOutbound)
	}, func(e error) int {
		fmt.Println(err)
		return -1
	})
	if err != nil {
		return err
	}

	<-ctx.Done()
	return nil
}
