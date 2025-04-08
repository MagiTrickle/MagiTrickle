package app

import (
	"context"
	"fmt"
	"net"
	"time"

	dnsMitmProxy "magitrickle/dns-mitm-proxy"
	"magitrickle/records"

	"github.com/miekg/dns"
	"github.com/rs/zerolog/log"
)

func (a *App) initDNSMITM() {
	a.dnsMITM = &dnsMitmProxy.DNSMITM{
		RequestHook:  a.dnsRequestHook,
		ResponseHook: a.dnsResponseHook,
	}
	a.records = records.New()
}

func (a *App) startDNSListeners(ctx context.Context, errChan chan error) {
	go func() {
		if err := a.dnsMITM.Serve(ctx); err != nil {
			errChan <- fmt.Errorf("failed to serve DNS MITM: %v", err)
		}
	}()
}

// dnsRequestHook обрабатывает входящие DNS-запросы
func (a *App) dnsRequestHook(clientAddr net.IP, dnsAddr net.IP, network string, reqMsg dns.Msg) (*dns.Msg, error) {
	var clientAddrStr string
	if clientAddr != nil {
		clientAddrStr = clientAddr.String()
	}
	for _, q := range reqMsg.Question {
		log.Trace().
			Str("name", q.Name).
			Int("qtype", int(q.Qtype)).
			Int("qclass", int(q.Qclass)).
			Str("clientAddr", clientAddrStr).
			Str("network", network).
			Msg("requested record")
	}

	return nil, nil
}

// dnsResponseHook обрабатывает ответы DNS
func (a *App) dnsResponseHook(clientAddr net.IP, dnsAddr net.IP, network string, respMsg dns.Msg) (*dns.Msg, error) {
	defer a.handleMessage(respMsg, clientAddr, &network)

	if a.config.DNSProxy.DisableDropAAAA {
		return nil, nil
	}

	// фильтрация записей AAAA
	var filteredAnswers []dns.RR
	for _, answer := range respMsg.Answer {
		if answer.Header().Rrtype != dns.TypeAAAA {
			filteredAnswers = append(filteredAnswers, answer)
		}
	}
	respMsg.Answer = filteredAnswers

	return &respMsg, nil
}

// handleMessage обрабатывает полученное DNS-сообщение
func (a *App) handleMessage(msg dns.Msg, clientAddr net.IP, network *string) {
	for _, rr := range msg.Answer {
		a.handleRecord(rr, clientAddr, network)
	}
}

// handleRecord маршрутизирует обработку DNS-записи в зависимости от её типа (A или CNAME)
func (a *App) handleRecord(rr dns.RR, clientAddr net.IP, network *string) {
	switch v := rr.(type) {
	case *dns.A:
		a.processARecord(*v, clientAddr, network)
	case *dns.CNAME:
		a.processCNameRecord(*v, clientAddr, network)
	}
}

func (a *App) processARecord(aRecord dns.A, clientAddr net.IP, network *string) {
	var clientAddrStr, networkStr string
	if clientAddr != nil {
		clientAddrStr = clientAddr.String()
	}
	if network != nil {
		networkStr = *network
	}
	log.Trace().
		Str("name", aRecord.Hdr.Name).
		Str("address", aRecord.A.String()).
		Int("ttl", int(aRecord.Hdr.Ttl)).
		Str("clientAddr", clientAddrStr).
		Str("network", networkStr).
		Msg("processing a record")

	ttlDuration := aRecord.Hdr.Ttl + a.config.Netfilter.IPSet.AdditionalTTL

	a.records.AddARecord(aRecord.Hdr.Name[:len(aRecord.Hdr.Name)-1], aRecord.A, ttlDuration)

	names := a.records.GetAliases(aRecord.Hdr.Name[:len(aRecord.Hdr.Name)-1])
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
				if err := group.AddIP(aRecord.A, ttlDuration); err != nil {
					log.Error().
						Str("address", aRecord.A.String()).
						Err(err).
						Msg("failed to add address")
				} else {
					log.Debug().
						Str("address", aRecord.A.String()).
						Str("aRecordDomain", aRecord.Hdr.Name).
						Str("cNameDomain", name).
						Msg("add address")
				}
				break Rule
			}
		}
	}
}

func (a *App) processCNameRecord(cNameRecord dns.CNAME, clientAddr net.IP, network *string) {
	var clientAddrStr, networkStr string
	if clientAddr != nil {
		clientAddrStr = clientAddr.String()
	}
	if network != nil {
		networkStr = *network
	}
	log.Trace().
		Str("name", cNameRecord.Hdr.Name).
		Str("cname", cNameRecord.Target).
		Int("ttl", int(cNameRecord.Hdr.Ttl)).
		Str("clientAddr", clientAddrStr).
		Str("network", networkStr).
		Msg("processing cname record")

	ttlDuration := cNameRecord.Hdr.Ttl + a.config.Netfilter.IPSet.AdditionalTTL

	a.records.AddCNameRecord(cNameRecord.Hdr.Name[:len(cNameRecord.Hdr.Name)-1],
		cNameRecord.Target[:len(cNameRecord.Target)-1],
		ttlDuration)

	now := time.Now()
	aRecords := a.records.GetARecords(cNameRecord.Hdr.Name[:len(cNameRecord.Hdr.Name)-1])
	names := a.records.GetAliases(cNameRecord.Hdr.Name[:len(cNameRecord.Hdr.Name)-1])
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
				for _, aRecord := range aRecords {
					if err := group.AddIP(aRecord.Address, uint32(now.Sub(aRecord.Deadline).Seconds())); err != nil {
						log.Error().
							Str("address", aRecord.Address.String()).
							Err(err).
							Msg("failed to add address")
					} else {
						log.Debug().
							Str("address", aRecord.Address.String()).
							Str("cNameDomain", name).
							Msg("add address")
					}
				}
				continue Rule
			}
		}
	}
}
