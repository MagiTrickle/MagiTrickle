// Command helper runs on the router during E2E tests.
//
//	helper dns  <listen> <zone.json>   scripted upstream DNS server (zone is re-read on every query)
//	helper http <listen> <dir>         static file server for subscription lists
package main

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"strings"

	"github.com/miekg/dns"
)

// Record is one zone entry: {"name":"a.test","type":"A","value":"192.0.2.1","ttl":60}.
// An entry with type "NXDOMAIN" makes the name answer with rcode NXDOMAIN.
type Record struct {
	Name  string `json:"name"`
	Type  string `json:"type"`
	Value string `json:"value"`
	TTL   uint32 `json:"ttl"`
}

func loadZone(path string) []Record {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var zone []Record
	_ = json.Unmarshal(data, &zone)
	return zone
}

func rr(r Record) dns.RR {
	ttl := r.TTL
	if ttl == 0 {
		ttl = 60
	}
	hdr := func(t uint16) dns.RR_Header {
		return dns.RR_Header{Name: dns.Fqdn(r.Name), Rrtype: t, Class: dns.ClassINET, Ttl: ttl}
	}
	switch strings.ToUpper(r.Type) {
	case "A":
		return &dns.A{Hdr: hdr(dns.TypeA), A: net.ParseIP(r.Value).To4()}
	case "AAAA":
		return &dns.AAAA{Hdr: hdr(dns.TypeAAAA), AAAA: net.ParseIP(r.Value)}
	case "CNAME":
		return &dns.CNAME{Hdr: hdr(dns.TypeCNAME), Target: dns.Fqdn(r.Value)}
	}
	return nil
}

func answer(zone []Record, name string, qtype uint16, m *dns.Msg) {
	name = strings.ToLower(dns.Fqdn(name))
	for depth := 0; depth < 8; depth++ {
		var next string
		for _, r := range zone {
			if strings.ToLower(dns.Fqdn(r.Name)) != name {
				continue
			}
			if strings.EqualFold(r.Type, "NXDOMAIN") {
				m.Rcode = dns.RcodeNameError
				return
			}
			rec := rr(r)
			if rec == nil {
				continue
			}
			t := rec.Header().Rrtype
			if t == dns.TypeCNAME {
				m.Answer = append(m.Answer, rec)
				next = strings.ToLower(dns.Fqdn(r.Value))
			} else if t == qtype {
				m.Answer = append(m.Answer, rec)
			}
		}
		if next == "" {
			return
		}
		name = next
	}
}

func main() {
	if len(os.Args) != 4 {
		fmt.Fprintln(os.Stderr, "usage: helper dns|http <listen> <zone.json|dir>")
		os.Exit(2)
	}
	switch os.Args[1] {
	case "dns":
		zonePath := os.Args[3]
		h := dns.HandlerFunc(func(w dns.ResponseWriter, req *dns.Msg) {
			m := new(dns.Msg)
			m.SetReply(req)
			m.RecursionAvailable = true
			if len(req.Question) == 1 {
				answer(loadZone(zonePath), req.Question[0].Name, req.Question[0].Qtype, m)
			}
			_ = w.WriteMsg(m)
		})
		errc := make(chan error, 2)
		for _, n := range []string{"udp", "tcp"} {
			srv := &dns.Server{Addr: os.Args[2], Net: n, Handler: h}
			go func() { errc <- srv.ListenAndServe() }()
		}
		fmt.Fprintln(os.Stderr, "helper dns:", <-errc)
		os.Exit(1)
	case "http":
		fmt.Fprintln(os.Stderr, "helper http:", http.ListenAndServe(os.Args[2], http.FileServer(http.Dir(os.Args[3]))))
		os.Exit(1)
	}
}
