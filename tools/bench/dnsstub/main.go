// dnsstub is a minimal fixed-answer upstream DNS server used as the upstream
// for MagiTrickle benchmark runs. It answers every A query with a fixed
// address (optionally behind a CNAME chain) with near-zero processing cost,
// so measured latency/throughput is dominated by the proxy under test.
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"strings"

	"github.com/miekg/dns"
)

var (
	listen   = flag.String("listen", "127.0.0.1:5399", "address to listen on (UDP and TCP)")
	cnameLen = flag.Int("cname", 0, "length of CNAME chain to prepend to every A answer")
	ttl      = flag.Uint("ttl", 300, "TTL for all answers")
	answerIP = flag.String("ip", "10.99.0.1", "IPv4 address returned in A answers")
)

func handler(w dns.ResponseWriter, req *dns.Msg) {
	resp := new(dns.Msg)
	resp.SetReply(req)
	resp.Authoritative = true

	if len(req.Question) == 1 {
		q := req.Question[0]
		switch q.Qtype {
		case dns.TypeA:
			owner := q.Name
			for i := 0; i < *cnameLen; i++ {
				target := fmt.Sprintf("c%d.%s", i, q.Name)
				resp.Answer = append(resp.Answer, &dns.CNAME{
					Hdr:    dns.RR_Header{Name: owner, Rrtype: dns.TypeCNAME, Class: dns.ClassINET, Ttl: uint32(*ttl)},
					Target: target,
				})
				owner = target
			}
			resp.Answer = append(resp.Answer, &dns.A{
				Hdr: dns.RR_Header{Name: owner, Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: uint32(*ttl)},
				A:   net.ParseIP(*answerIP).To4(),
			})
		case dns.TypeAAAA:
			resp.Answer = append(resp.Answer, &dns.AAAA{
				Hdr:  dns.RR_Header{Name: q.Name, Rrtype: dns.TypeAAAA, Class: dns.ClassINET, Ttl: uint32(*ttl)},
				AAAA: net.ParseIP("fd00::1"),
			})
		default:
			resp.Rcode = dns.RcodeNameError
		}
	}
	_ = w.WriteMsg(resp)
}

func main() {
	flag.Parse()
	dns.HandleFunc(".", handler)

	errCh := make(chan error, 2)
	for _, proto := range []string{"udp", "tcp"} {
		srv := &dns.Server{Addr: *listen, Net: proto}
		go func() { errCh <- srv.ListenAndServe() }()
	}
	fmt.Fprintf(os.Stderr, "dnsstub: listening on %s (udp+tcp), cname=%d\n", *listen, *cnameLen)
	err := <-errCh
	if err != nil && !strings.Contains(err.Error(), "use of closed") {
		log.Fatal(err)
	}
}
