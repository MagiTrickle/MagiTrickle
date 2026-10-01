// configbench times config load/save of the Go backend in-process.
// It reads the config from the platform default path (for a host build:
// /var/lib/magitrickle/config.yaml), so generate the desired config there
// first with genconfig. The daemon must NOT be running (no netfilter side
// effects happen while the app core is not started).
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"time"

	"magitrickle"
)

type out struct {
	Runs       int       `json:"runs"`
	LoadMs     []float64 `json:"load_ms"`
	SaveMs     []float64 `json:"save_ms"`
	RuleCount  int       `json:"rule_count"`
	GroupCount int       `json:"group_count"`
}

func main() {
	const runs = 5
	res := out{Runs: runs}

	app := magitrickle.New() // first load included in New(); timed loads below

	for i := 0; i < runs; i++ {
		t0 := time.Now()
		if err := app.LoadConfig(); err != nil {
			fmt.Fprintln(os.Stderr, "load:", err)
			os.Exit(1)
		}
		res.LoadMs = append(res.LoadMs, float64(time.Since(t0).Microseconds())/1000.0)

		t0 = time.Now()
		if err := app.SaveConfig(); err != nil {
			fmt.Fprintln(os.Stderr, "save:", err)
			os.Exit(1)
		}
		res.SaveMs = append(res.SaveMs, float64(time.Since(t0).Microseconds())/1000.0)
	}

	for _, g := range app.UserGroups() {
		res.GroupCount++
		if m := g.Model(); m != nil {
			res.RuleCount += len(m.Rules)
		}
	}

	_ = json.NewEncoder(os.Stdout).Encode(res)
}
