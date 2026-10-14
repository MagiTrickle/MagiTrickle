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
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
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
	corpus      = flag.String("corpus", "", "file of real query names (one per line; # comments)")
	rate        = flag.Float64("qps", 0, "aggregate offered queries/sec (0 = capacity)")
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
	ElapsedSec  float64 `json:"elapsed_sec"`
	Invalid     int64   `json:"invalid"`
	OfferedQPS  float64 `json:"offered_qps"`
	CPUPercent  float64 `json:"loader_cpu_pct"`
}

// A DNS error response is not a successful benchmark query. Accept real CNAME
// chains, but require at least one A answer and the exact echoed question.
func validResponse(query, response *dns.Msg) bool {
	if response == nil || !response.Response || response.Id != query.Id ||
		response.Opcode != dns.OpcodeQuery || response.Rcode != dns.RcodeSuccess ||
		response.Truncated || len(response.Question) != 1 ||
		!strings.EqualFold(response.Question[0].Name, query.Question[0].Name) ||
		response.Question[0].Qtype != dns.TypeA || response.Question[0].Qclass != dns.ClassINET {
		return false
	}
	for _, answer := range response.Answer {
		if a, ok := answer.(*dns.A); ok && a.Hdr.Class == dns.ClassINET && a.A.To4() != nil {
			return true
		}
	}
	return false
}

func cpuSeconds() float64 {
	var usage syscall.Rusage
	if err := syscall.Getrusage(syscall.RUSAGE_SELF, &usage); err != nil {
		return 0
	}
	return float64(usage.Utime.Sec+usage.Stime.Sec) + float64(usage.Utime.Usec+usage.Stime.Usec)/1e6
}

func query(c *dns.Client, name string) (time.Duration, error) {
	m := new(dns.Msg)
	m.SetQuestion(name, dns.TypeA)
	m.Id = dns.Id()
	response, rtt, err := c.Exchange(m, *server)
	if err == nil && !validResponse(m, response) {
		err = fmt.Errorf("invalid DNS answer")
	}
	return rtt, err
}

func probe(name string) {
	c := &dns.Client{Net: *proto, Timeout: *timeout}
	start := time.Now()
	for time.Since(start) < *probeLimit {
		if _, err := query(c, name); err == nil {
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
	if *concurrency < 1 || *nDomains < 1 || *duration <= 0 || *rate < 0 || (*proto != "udp" && *proto != "tcp") {
		fmt.Fprintln(os.Stderr, "invalid load parameters")
		os.Exit(2)
	}
	var names []string
	if *corpus != "" {
		data, err := os.ReadFile(*corpus)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
		for _, line := range strings.Split(string(data), "\n") {
			name := strings.TrimSpace(line)
			if name != "" && !strings.HasPrefix(name, "#") {
				if _, ok := dns.IsDomainName(name); !ok {
					fmt.Fprintln(os.Stderr, "invalid corpus name:", name)
					os.Exit(2)
				}
				names = append(names, dns.Fqdn(name))
			}
		}
		if len(names) == 0 {
			fmt.Fprintln(os.Stderr, "empty corpus")
			os.Exit(2)
		}
		*nDomains = len(names)
	}
	if *mode == "probe" {
		name := fmt.Sprintf(*pattern, 0)
		if len(names) > 0 {
			name = names[0]
		}
		probe(name)
		return
	}

	var sent, ok, errs, timeouts, invalid int64
	latencies := make([][]time.Duration, *concurrency)
	start := time.Now()
	cpuStart := cpuSeconds()
	deadline := start.Add(*duration)

	var wg sync.WaitGroup
	for w := 0; w < *concurrency; w++ {
		latencies[w] = make([]time.Duration, 0, 1<<16)
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
			sequence := 0
			for time.Now().Before(deadline) {
				if *rate > 0 {
					due := start.Add(time.Duration(float64(w+sequence*(*concurrency)) / *rate * float64(time.Second)))
					if !due.Before(deadline) {
						break
					}
					time.Sleep(time.Until(due))
				}
				sequence++
				i := rng.Intn(*nDomains)
				name := fmt.Sprintf(*pattern, i)
				if len(names) > 0 {
					name = names[i]
				}
				m := new(dns.Msg)
				m.SetQuestion(name, dns.TypeA)
				m.Id = dns.Id()
				atomic.AddInt64(&sent, 1)
				var rtt time.Duration
				var qerr error
				var resp *dns.Msg
				queryStart := time.Now()
				if *proto == "udp" {
					resp, rtt, qerr = c.ExchangeWithConn(m, conn)
				} else {
					// TCP: proxy closes the connection after one exchange
					resp, rtt, qerr = c.Exchange(m, *server)
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
				if !validResponse(m, resp) {
					atomic.AddInt64(&invalid, 1)
					atomic.AddInt64(&errs, 1)
					continue
				}
				atomic.AddInt64(&ok, 1)
				// Include TCP connect and response validation in client latency.
				// Exchange's own RTT starts after dialing the connection.
				_ = rtt
				local = append(local, time.Since(queryStart))
			}
			latencies[w] = local
		}(w)
	}
	wg.Wait()
	elapsed := time.Since(start).Seconds()
	cpuElapsed := cpuSeconds() - cpuStart

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
		RPS:         float64(ok) / elapsed,
		P50ms:       pct(0.50),
		P95ms:       pct(0.95),
		P99ms:       pct(0.99),
		MaxMs:       pct(1.0),
		ElapsedSec:  elapsed,
		Invalid:     invalid,
		OfferedQPS:  *rate,
		CPUPercent:  cpuElapsed / elapsed * 100,
	}
	enc := json.NewEncoder(os.Stdout)
	_ = enc.Encode(res)
}
