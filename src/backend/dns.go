package magitrickle

import (
	"context"
	"fmt"
	"net"
	"time"

	"magitrickle/routing"

	"github.com/miekg/dns"
	"github.com/rs/zerolog/log"
)

var hexDigits = []byte("0123456789abcdef")

func formatID(id uint16) string {
	return string([]byte{
		hexDigits[id>>12&0xf],
		hexDigits[id>>8&0xf],
		hexDigits[id>>4&0xf],
		hexDigits[id&0xf],
	})
}

func trimFQDN(name string) string {
	if len(name) > 0 && name[len(name)-1] == '.' {
		return name[:len(name)-1]
	}
	return name
}

func (a *App) startDNSListeners(ctx context.Context, errChan chan error) {
	go func() {
		addr, err := net.ResolveUDPAddr("udp", fmt.Sprintf("%s:%d", a.cfg().DNSProxy.Host.Address, a.cfg().DNSProxy.Host.Port))
		if err != nil {
			errChan <- fmt.Errorf("failed to resolve udp address: %v", err)
			return
		}
		if err = a.dnsMITM.ListenUDP(ctx, addr); err != nil {
			errChan <- fmt.Errorf("failed to serve DNS UDP proxy: %v", err)
		}
	}()

	go func() {
		addr, err := net.ResolveTCPAddr("tcp", fmt.Sprintf("%s:%d", a.cfg().DNSProxy.Host.Address, a.cfg().DNSProxy.Host.Port))
		if err != nil {
			errChan <- fmt.Errorf("failed to resolve tcp address: %v", err)
			return
		}
		if err = a.dnsMITM.ListenTCP(ctx, addr); err != nil {
			errChan <- fmt.Errorf("failed to serve DNS TCP proxy: %v", err)
		}
	}()
}

// dnsRequestHook обрабатывает входящие DNS-запросы
func (a *App) dnsRequestHook(clientAddr net.Addr, reqMsg dns.Msg, network string) (*dns.Msg, *dns.Msg, error) {
	var clientAddrStr string
	if clientAddr != nil {
		clientAddrStr = clientAddr.String()
	}
	idStr := formatID(reqMsg.Id)

	log.Debug().
		Str("id", idStr).
		Str("clientAddr", clientAddrStr).
		Str("network", network).
		Msg("request received")

	for _, q := range reqMsg.Question {
		log.Info().
			Str("id", idStr).
			Str("name", q.Name).
			Int("qtype", int(q.Qtype)).
			Int("qclass", int(q.Qclass)).
			Str("clientAddr", clientAddrStr).
			Str("network", network).
			Msg("requested record")
	}

	if a.cfg().DNSProxy.DisableFakePTR {
		return nil, nil, nil
	}

	if len(reqMsg.Question) == 1 && reqMsg.Question[0].Qtype == dns.TypePTR {
		respMsg := &dns.Msg{
			MsgHdr: dns.MsgHdr{
				Id:                 reqMsg.Id,
				Response:           true,
				RecursionAvailable: true,
				Rcode:              dns.RcodeNameError,
			},
			Question: reqMsg.Question,
		}
		return nil, respMsg, nil
	}

	return nil, nil, nil
}

// dnsResponseHook обрабатывает ответы DNS
func (a *App) dnsResponseHook(clientAddr net.Addr, reqMsg dns.Msg, respMsg dns.Msg, network string) (*dns.Msg, error) {
	if a.cfg().DNSProxy.DisableDropAAAA {
		a.handleMessage(respMsg, clientAddr, network)
		return nil, nil
	}

	// фильтрация записей AAAA
	filteredAnswers := make([]dns.RR, 0, len(respMsg.Answer))
	for _, answer := range respMsg.Answer {
		if answer.Header().Rrtype != dns.TypeAAAA {
			filteredAnswers = append(filteredAnswers, answer)
		}
	}
	respMsg.Answer = filteredAnswers

	// Only what the client will actually receive is routed: a dropped AAAA must not end up in an ipset.
	a.handleMessage(respMsg, clientAddr, network)

	return &respMsg, nil
}

// handleMessage обрабатывает полученное DNS-сообщение
func (a *App) handleMessage(msg dns.Msg, clientAddr net.Addr, network string) {
	idStr := formatID(msg.Id)
	var clientAddrStr string
	if clientAddr != nil {
		clientAddrStr = clientAddr.String()
	}

	if msg.Rcode != dns.RcodeSuccess {
		log.Warn().
			Str("id", idStr).
			Str("clientAddr", clientAddrStr).
			Str("network", network).
			Msg("unprocessable response")

		return
	}

	for _, rr := range msg.Answer {
		if rr == nil {
			continue
		}

		switch v := rr.(type) {
		case *dns.A:
			a.processARecord(*v, idStr, clientAddrStr, network)
		case *dns.AAAA:
			a.processAAAARecord(*v, idStr, clientAddrStr, network)
		case *dns.CNAME:
			a.processCNameRecord(*v, idStr, clientAddrStr, network)
		}
	}
}

func (a *App) processARecord(aRecord dns.A, idStr, clientAddrStr, network string) {
	domainName := trimFQDN(aRecord.Hdr.Name)
	addrStr := aRecord.A.String()

	if len(aRecord.A) != 4 {
		log.Warn().
			Str("id", idStr).
			Str("name", domainName).
			Str("address", addrStr).
			Int("ttl", int(aRecord.Hdr.Ttl)).
			Str("clientAddr", clientAddrStr).
			Str("network", network).
			Msg("unprocessable A response")
		return
	}

	log.Debug().
		Str("id", idStr).
		Str("name", domainName).
		Str("address", addrStr).
		Int("ttl", int(aRecord.Hdr.Ttl)).
		Str("clientAddr", clientAddrStr).
		Str("network", network).
		Msg("processing A record")

	ttlDuration := aRecord.Hdr.Ttl + uint32(a.cfg().Netfilter.IPSet.AdditionalTTL.Seconds())

	a.recordsCache.AddAddress(domainName, aRecord.A, ttlDuration)

	if table := a.routing.Load(); table != nil {
		table.AddAddresses(a.recordsCache.GetAliases(domainName), []routing.Address{{IP: aRecord.A, TTL: ttlDuration}})
	}
}

func (a *App) processAAAARecord(aaaaRecord dns.AAAA, idStr, clientAddrStr, network string) {
	domainName := trimFQDN(aaaaRecord.Hdr.Name)
	addrStr := aaaaRecord.AAAA.String()

	if len(aaaaRecord.AAAA) != 16 {
		log.Warn().
			Str("id", idStr).
			Str("name", domainName).
			Str("address", addrStr).
			Int("ttl", int(aaaaRecord.Hdr.Ttl)).
			Str("clientAddr", clientAddrStr).
			Str("network", network).
			Msg("unprocessable AAAA response")
		return
	}

	log.Debug().
		Str("id", idStr).
		Str("name", domainName).
		Str("address", addrStr).
		Int("ttl", int(aaaaRecord.Hdr.Ttl)).
		Str("clientAddr", clientAddrStr).
		Str("network", network).
		Msg("processing AAAA record")

	ttlDuration := aaaaRecord.Hdr.Ttl + uint32(a.cfg().Netfilter.IPSet.AdditionalTTL.Seconds())

	a.recordsCache.AddAddress(domainName, aaaaRecord.AAAA, ttlDuration)

	if table := a.routing.Load(); table != nil {
		table.AddAddresses(a.recordsCache.GetAliases(domainName), []routing.Address{{IP: aaaaRecord.AAAA, TTL: ttlDuration}})
	}
}

func (a *App) processCNameRecord(cNameRecord dns.CNAME, idStr, clientAddrStr, network string) {
	domainName := trimFQDN(cNameRecord.Hdr.Name)
	targetName := trimFQDN(cNameRecord.Target)

	log.Debug().
		Str("id", idStr).
		Str("name", domainName).
		Str("cname", targetName).
		Int("ttl", int(cNameRecord.Hdr.Ttl)).
		Str("clientAddr", clientAddrStr).
		Str("network", network).
		Msg("processing CNAME record")

	ttlDuration := cNameRecord.Hdr.Ttl + uint32(a.cfg().Netfilter.IPSet.AdditionalTTL.Seconds())

	a.recordsCache.AddAlias(domainName, targetName, ttlDuration)

	table := a.routing.Load()
	if table == nil {
		return
	}

	// Адреса цели уже могли быть в кэше: направляем их через группы,
	// которые совпали с новым псевдонимом.
	now := time.Now()
	var addresses []routing.Address
	for _, address := range a.recordsCache.GetAddresses(domainName) {
		if ttl := address.Deadline.Sub(now).Seconds(); ttl > 0 {
			addresses = append(addresses, routing.Address{IP: address.Address, TTL: uint32(ttl)})
		}
	}
	table.AddAddresses(a.recordsCache.GetAliases(domainName), addresses)
}
