package magitrickle

import (
	"context"
	"fmt"
	"net"
	"time"

	"magitrickle/utils/netfilterTools"

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
		addr, err := net.ResolveUDPAddr("udp", fmt.Sprintf("%s:%d", a.config.DNSProxy.Host.Address, a.config.DNSProxy.Host.Port))
		if err != nil {
			errChan <- fmt.Errorf("failed to resolve udp address: %v", err)
			return
		}
		if err = a.dnsMITM.ListenUDP(ctx, addr); err != nil {
			errChan <- fmt.Errorf("failed to serve DNS UDP proxy: %v", err)
		}
	}()

	go func() {
		addr, err := net.ResolveTCPAddr("tcp", fmt.Sprintf("%s:%d", a.config.DNSProxy.Host.Address, a.config.DNSProxy.Host.Port))
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

	if a.config.DNSProxy.DisableFakePTR {
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
	defer a.handleMessage(respMsg, clientAddr, network)

	if a.config.DNSProxy.DisableDropAAAA {
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

	ttlDuration := aRecord.Hdr.Ttl + a.config.Netfilter.IPSet.AdditionalTTL

	a.recordsCache.AddAddress(domainName, aRecord.A, ttlDuration)

	names := a.recordsCache.GetAliases(domainName)
	for _, group := range a.groups {
	Rule:
		for _, domain := range group.Rules {
			if !domain.IsEnabled() {
				continue
			}
			for _, name := range names {
				if !domain.IsMatch(name) {
					continue
				}

				// TODO: Check already existed
				subnet := netfilterTools.IPv4Subnet{Address: [4]byte(aRecord.A)}
				if err := group.AddIPv4Subnet(subnet, &ttlDuration); err != nil {
					log.Error().
						Err(err).
						Str("subnet", subnet.String()).
						Str("aRecordDomain", domainName).
						Str("cNameDomain", name).
						Msg("failed to add subnet")
				} else {
					log.Debug().
						Str("subnet", subnet.String()).
						Str("aRecordDomain", domainName).
						Str("cNameDomain", name).
						Msg("added subnet")
				}

				log.Info().
					Str("name", domainName).
					Str("address", addrStr).
					Str("group", group.Name).
					Str("groupId", group.ID.String()).
					Msg("added to routing")

				break Rule
			}
		}
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

	ttlDuration := aaaaRecord.Hdr.Ttl + a.config.Netfilter.IPSet.AdditionalTTL

	a.recordsCache.AddAddress(domainName, aaaaRecord.AAAA, ttlDuration)

	names := a.recordsCache.GetAliases(domainName)
	for _, group := range a.groups {
	Rule:
		for _, domain := range group.Rules {
			if !domain.IsEnabled() {
				continue
			}
			for _, name := range names {
				if !domain.IsMatch(name) {
					continue
				}

				// TODO: Check already existed
				subnet := netfilterTools.IPv6Subnet{Address: [16]byte(aaaaRecord.AAAA)}
				if err := group.AddIPv6Subnet(subnet, &ttlDuration); err != nil {
					log.Error().
						Err(err).
						Str("subnet", subnet.String()).
						Str("aaaaRecordDomain", domainName).
						Str("cNameDomain", name).
						Msg("failed to add subnet")
				} else {
					log.Debug().
						Str("subnet", subnet.String()).
						Str("aaaaRecordDomain", domainName).
						Str("cNameDomain", name).
						Msg("added subnet")
				}

				log.Info().
					Str("name", domainName).
					Str("address", addrStr).
					Str("group", group.Name).
					Str("groupId", group.ID.String()).
					Msg("added to routing")

				break Rule
			}
		}
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

	ttlDuration := cNameRecord.Hdr.Ttl + a.config.Netfilter.IPSet.AdditionalTTL

	a.recordsCache.AddAlias(domainName, targetName, ttlDuration)

	now := time.Now()
	addresses := a.recordsCache.GetAddresses(domainName)
	aliases := a.recordsCache.GetAliases(domainName)
	for _, group := range a.groups {
	Rule:
		for _, domain := range group.Rules {
			if !domain.IsEnabled() {
				continue
			}
			for _, alias := range aliases {
				if !domain.IsMatch(alias) {
					continue
				}

				log.Info().
					Str("name", domainName).
					Str("cname", targetName).
					Str("group", group.Name).
					Str("groupId", group.ID.String()).
					Msg("added alias")

				for _, address := range addresses {
					ttlDuration := address.Deadline.Sub(now).Seconds()
					if ttlDuration <= 0 {
						continue
					}
					ttl := uint32(ttlDuration)

					if len(address.Address) == net.IPv4len {
						subnet := netfilterTools.IPv4Subnet{Address: [4]byte(address.Address)}
						if err := group.AddIPv4Subnet(subnet, &ttl); err != nil {
							log.Error().
								Err(err).
								Str("subnet", subnet.String()).
								Str("cNameDomain", alias).
								Msg("failed to add subnet")
						}
						log.Debug().
							Str("subnet", subnet.String()).
							Str("cNameDomain", alias).
							Msg("added subnet")
					} else if len(address.Address) == net.IPv6len {
						subnet := netfilterTools.IPv6Subnet{Address: [16]byte(address.Address)}
						if err := group.AddIPv6Subnet(subnet, &ttl); err != nil {
							log.Error().
								Err(err).
								Str("subnet", subnet.String()).
								Str("cNameDomain", alias).
								Msg("failed to add subnet")
						}
						log.Debug().
							Str("subnet", subnet.String()).
							Str("cNameDomain", alias).
							Msg("added subnet")
					}
				}
				continue Rule
			}
		}
	}
}
