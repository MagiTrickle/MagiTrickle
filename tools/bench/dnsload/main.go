// dnsload is a DNS load generator used to measure MagiTrickle DNS proxy
// throughput and latency. It supports UDP (persistent socket per worker) and
// TCP (one connection per query — matching the proxy's one-query-per-TCP-conn
// behaviour), fixed concurrency, fixed duration, and reports rps and latency
// percentiles as JSON on stdout.
//
// Modes:
//
//	-mode probe : send a single query with retries until an answer arrives,
//	              print time-to-first-answer in ms (used to measure startup).
//	-mode load  : fixed-duration load run (default).
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"math/rand"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"

	"github.com/miekg/dns"
)

var (
	server      = flag.String("server", "127.0.0.1:3553", "DNS server under test")
	proto       = flag.String("proto", "udp", "udp or tcp")
	concurrency = flag.Int("concurrency", 10, "number of concurrent workers")
	duration    = flag.Duration("duration", 5*time.Second, "load duration")
	nDomains    = flag.Int("ndomains", 1000, "number of distinct query names")
	pattern     = flag.String("pattern", "d%06d.bench.example.com.", "query name pattern (must keep trailing dot)")
	timeout     = flag.Duration("timeout", 2*time.Second, "per-query timeout")
	mode        = flag.String("mode", "load", "load or probe")
	probeLimit  = flag.Duration("probe-limit", 30*time.Second, "max total time for probe mode")
	label       = flag.String("label", "", "free-form label copied into the JSON output")
)

type result struct {
	Label       string  `json:"label,omitempty"`
	Proto       string  `json:"proto"`
	Concurrency int     `json:"concurrency"`
	DurationSec float64 `json:"duration_sec"`
	NDomains    int     `json:"ndomains"`
	Sent        int64   `json:"sent"`
	OK          int64   `json:"ok"`
	Errors      int64   `json:"errors"`
	Timeouts    int64   `json:"timeouts"`
	RPS         float64 `json:"rps"`
	P50ms       float64 `json:"p50_ms"`
	P95ms       float64 `json:"p95_ms"`
	P99ms       float64 `json:"p99_ms"`
	MaxMs       float64 `json:"max_ms"`
}

func query(c *dns.Client, name string) (time.Duration, error) {
	m := new(dns.Msg)
	m.SetQuestion(name, dns.TypeA)
	m.Id = dns.Id()
	_, rtt, err := c.Exchange(m, *server)
	return rtt, err
}

func probe() {
	c := &dns.Client{Net: *proto, Timeout: *timeout}
	start := time.Now()
	for time.Since(start) < *probeLimit {
		if _, err := query(c, fmt.Sprintf(*pattern, 0)); err == nil {
			fmt.Printf("%.1f\n", float64(time.Since(start).Microseconds())/1000.0)
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	fmt.Fprintln(os.Stderr, "probe: no answer within limit")
	os.Exit(1)
}

func main() {
	flag.Parse()
	if *mode == "probe" {
		probe()
		return
	}

	var sent, ok, errs, timeouts int64
	latencies := make([][]time.Duration, *concurrency)
	deadline := time.Now().Add(*duration)

	var wg sync.WaitGroup
	for w := 0; w < *concurrency; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			rng := rand.New(rand.NewSource(int64(w)))
			c := &dns.Client{Net: *proto, Timeout: *timeout}
			var conn *dns.Conn
			var err error
			if *proto == "udp" {
				// persistent UDP socket per worker
				conn, err = c.Dial(*server)
				if err != nil {
					atomic.AddInt64(&errs, 1)
					return
				}
				defer conn.Close()
			}
			local := latencies[w][:0]
			for time.Now().Before(deadline) {
				name := fmt.Sprintf(*pattern, rng.Intn(*nDomains))
				m := new(dns.Msg)
				m.SetQuestion(name, dns.TypeA)
				m.Id = dns.Id()
				atomic.AddInt64(&sent, 1)
				var rtt time.Duration
				var qerr error
				if *proto == "udp" {
					var resp *dns.Msg
					resp, rtt, qerr = c.ExchangeWithConn(m, conn)
					_ = resp
				} else {
					// TCP: proxy closes the connection after one exchange
					_, rtt, qerr = c.Exchange(m, *server)
				}
				if qerr != nil {
					if os.IsTimeout(qerr) {
						atomic.AddInt64(&timeouts, 1)
					}
					atomic.AddInt64(&errs, 1)
					if *proto == "udp" {
						// resync socket after timeout
						conn.Close()
						conn, err = c.Dial(*server)
						if err != nil {
							return
						}
					}
					continue
				}
				atomic.AddInt64(&ok, 1)
				local = append(local, rtt)
			}
			latencies[w] = local
		}(w)
		latencies[w] = make([]time.Duration, 0, 1<<16)
	}
	wg.Wait()

	var all []time.Duration
	for _, l := range latencies {
		all = append(all, l...)
	}
	sort.Slice(all, func(i, j int) bool { return all[i] < all[j] })
	pct := func(p float64) float64 {
		if len(all) == 0 {
			return 0
		}
		idx := int(p * float64(len(all)-1))
		return float64(all[idx].Microseconds()) / 1000.0
	}

	res := result{
		Label:       *label,
		Proto:       *proto,
		Concurrency: *concurrency,
		DurationSec: duration.Seconds(),
		NDomains:    *nDomains,
		Sent:        sent,
		OK:          ok,
		Errors:      errs,
		Timeouts:    timeouts,
		RPS:         float64(ok) / duration.Seconds(),
		P50ms:       pct(0.50),
		P95ms:       pct(0.95),
		P99ms:       pct(0.99),
		MaxMs:       pct(1.0),
	}
	enc := json.NewEncoder(os.Stdout)
	_ = enc.Encode(res)
}
