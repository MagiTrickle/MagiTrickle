#!/usr/bin/env python3
"""Summarize run_baseline.sh results.jsonl into markdown tables.

Usage: python3 tools/bench/summarize.py docs/c-rewrite/baseline-raw/results.jsonl
Groups load cells by (rules, rule_type, scenario, proto, concurrency) and
prints median / min / max over repeats for rps and latency percentiles.
"""
import json
import statistics
import sys
from collections import defaultdict


def med(xs):
    return statistics.median(xs)


def main(path):
    cells = defaultdict(list)
    other = []
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        r = json.loads(line)
        if r.get("metric") or "cname_chain" in r or r.get("scenario") == "upstream_down" or r.get("label") == "soak":
            other.append(r)
            continue
        key = (r.get("rules"), r.get("rule_type"), r.get("scenario"),
               r.get("proto"), r.get("concurrency"))
        cells[key].append(r)

    print("| rules | type | scenario | proto | conc | rps med (min–max) | p50 ms | p95 ms | p99 ms | cpu% | rss MB |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for key in sorted(cells, key=lambda k: (k[0] or 0, k[1] or "", k[2] or "", k[3] or "", k[4] or 0)):
        rs = cells[key]
        rps = [x["rps"] for x in rs]
        p50 = [x["p50_ms"] for x in rs]
        p95 = [x["p95_ms"] for x in rs]
        p99 = [x["p99_ms"] for x in rs]
        cpu = [x.get("cpu_pct", 0) for x in rs]
        rss = [x.get("rss_kb", 0) / 1024 for x in rs]
        print(f"| {key[0]} | {key[1]} | {key[2]} | {key[3]} | {key[4]} "
              f"| {med(rps):.0f} ({min(rps):.0f}–{max(rps):.0f}) "
              f"| {med(p50):.2f} | {med(p95):.2f} | {med(p99):.2f} "
              f"| {med(cpu):.0f} | {med(rss):.1f} |")

    print("\n### Other records\n")
    for r in other:
        print("- `%s`" % json.dumps(r, sort_keys=True))


if __name__ == "__main__":
    main(sys.argv[1])
