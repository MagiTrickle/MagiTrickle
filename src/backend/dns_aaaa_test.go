package magitrickle

import (
	"net"
	"testing"

	"magitrickle/models"
	"magitrickle/utils/recordsCache"

	"github.com/miekg/dns"
)

func aaaaResponse() dns.Msg {
	var m dns.Msg
	m.SetQuestion("v6.example.", dns.TypeAAAA)
	m.Response = true
	m.Answer = []dns.RR{
		&dns.A{Hdr: dns.RR_Header{Name: "v4.example.", Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 60}, A: net.ParseIP("203.0.113.1").To4()},
		&dns.AAAA{Hdr: dns.RR_Header{Name: "v6.example.", Rrtype: dns.TypeAAAA, Class: dns.ClassINET, Ttl: 60}, AAAA: net.ParseIP("2001:db8::1")},
	}
	return m
}

func TestDNSResponseHookDoesNotProcessDroppedAAAA(t *testing.T) {
	app := &App{recordsCache: recordsCache.New()}

	resp, err := app.dnsResponseHook(nil, dns.Msg{}, aaaaResponse(), "udp")
	if err != nil {
		t.Fatal(err)
	}
	if resp == nil || len(resp.Answer) != 1 {
		t.Fatalf("client must get only the A record, got %+v", resp)
	}
	if got := app.recordsCache.GetAddresses("v4.example"); len(got) != 1 {
		t.Errorf("A record not processed: %v", got)
	}
	if got := app.recordsCache.GetAddresses("v6.example"); len(got) != 0 {
		t.Errorf("dropped AAAA record was processed: %v", got)
	}
}

func TestDNSResponseHookProcessesAAAAWhenNotDropped(t *testing.T) {
	app := &App{recordsCache: recordsCache.New(), config: models.AppConfig{}}
	app.config.DNSProxy.DisableDropAAAA = true

	resp, err := app.dnsResponseHook(nil, dns.Msg{}, aaaaResponse(), "udp")
	if err != nil || resp != nil {
		t.Fatalf("response must be passed through untouched, got %v, %v", resp, err)
	}
	if got := app.recordsCache.GetAddresses("v6.example"); len(got) != 1 {
		t.Errorf("AAAA record not processed: %v", got)
	}
}
