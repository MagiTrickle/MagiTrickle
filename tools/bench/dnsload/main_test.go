package main

import (
	"net"
	"testing"

	"github.com/miekg/dns"
)

func TestValidResponse(t *testing.T) {
	query := new(dns.Msg)
	query.SetQuestion("example.com.", dns.TypeA)
	response := new(dns.Msg)
	response.SetReply(query)
	response.Answer = []dns.RR{&dns.A{
		Hdr: dns.RR_Header{Name: "example.com.", Rrtype: dns.TypeA, Class: dns.ClassINET},
		A:   net.ParseIP("192.0.2.1"),
	}}
	if !validResponse(query, response) {
		t.Fatal("valid A answer rejected")
	}
	for _, mutate := range []func(*dns.Msg){
		func(r *dns.Msg) { r.Rcode = dns.RcodeServerFailure },
		func(r *dns.Msg) { r.Rcode = dns.RcodeNameError },
		func(r *dns.Msg) { r.Id++ },
		func(r *dns.Msg) { r.Question[0].Name = "other.com." },
		func(r *dns.Msg) { r.Answer = nil },
		func(r *dns.Msg) { r.Truncated = true },
	} {
		r := response.Copy()
		mutate(r)
		if validResponse(query, r) {
			t.Fatal("invalid answer counted as successful")
		}
	}
	if validResponse(query, nil) {
		t.Fatal("nil response accepted")
	}
}
