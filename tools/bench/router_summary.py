#!/usr/bin/env python3
"""Summarize router_dns.py JSONL, preserving repeat spread and failures."""
import argparse
import collections
import json
import statistics
from pathlib import Path


def summarize(path):
    paths = [path] if isinstance(path, Path) else path
    rows = [json.loads(line) for path in paths for line in path.read_text().splitlines() if line.strip()]
    cells = collections.defaultdict(list)
    for row in rows:
        if row["metric"] == "load":
            cells[(row["rules"], row["scenario"], row["proto"], row["backend"])].append(row)
    print("| Правила | Сценарий | DNS | Пакет | Прогоны | RPS, медиана (min–max) | p50 / p95 / p99, мс | CPU, % (min–max) | RSS, MiB | VmHWM / VmSwap, MiB | CPU генератора, % | Ошибки / таймауты |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for key, cell in sorted(cells.items()):
        med = lambda field: statistics.median(r[field] for r in cell)
        rps = [r["rps"] for r in cell]
        cpu = [r["cpu_pct"] for r in cell]
        rss = statistics.median(r["after"]["VmRSS"] / 1024 for r in cell)
        hwm = statistics.median(r["after"]["VmHWM"] / 1024 for r in cell)
        swap = (f"{statistics.median(r['after']['VmSwap'] / 1024 for r in cell):.2f}"
                if all("VmSwap" in r["after"] for r in cell) else "—")
        print(f"| {' | '.join(str(x) for x in key)} | {len(cell)} | "
              f"{med('rps'):.0f} ({min(rps):.0f}–{max(rps):.0f}) | "
              f"{med('p50_ms'):.2f} / {med('p95_ms'):.2f} / {med('p99_ms'):.2f} | "
              f"{med('cpu_pct'):.1f} ({min(cpu):.1f}–{max(cpu):.1f}) | {rss:.2f} | {hwm:.2f} / {swap} | "
              f"{med('loader_cpu_pct'):.1f} | {sum(r['errors'] for r in cell)} / "
              f"{sum(r['timeouts'] for r in cell)} |")
    print("\nТаймауты входят в ошибки; ошибки не включены в RPS и процентили задержек.")
    failures = [r for r in rows if r["metric"] in ("failure", "setup_failure")]
    if failures:
        print("\n| Правила | Пакет | Остановленные попытки | Причина |")
        print("|---|---|---|---|")
        failed = collections.Counter((r["rules"], r["backend"], r["reason"]) for r in failures)
        for (rules, backend, reason), count in sorted(failed.items()):
            print(f"| {rules} | {backend} | {count} | {reason}; RPS не измерен |")
    # Fit observed median CPU cost against N, not RPS against log(N).
    # This is an empirical description within the sampled range only.
    fit = []
    for (rules, scenario, proto, backend), cell in sorted(cells.items()):
        if scenario == "capacity" and proto == "udp" and backend == "original" and len(cell) == 3:
            cost = statistics.median(r["cpu_pct"] / 100 * r["cpu_interval_sec"] / r["ok"] * 1000 for r in cell)
            fit.append((rules, cost))
    if len(fit) >= 4:
        xm = statistics.mean(x for x, y in fit)
        ym = statistics.mean(y for x, y in fit)
        slope = sum((x-xm)*(y-ym) for x, y in fit) / sum((x-xm)**2 for x, y in fit)
        intercept = ym - slope * xm
        r2 = 1 - sum((y-intercept-slope*x)**2 for x, y in fit) / sum((y-ym)**2 for x, y in fit)
        print(f"\nЭмпирическая модель Go, {fit[0][0]}–{fit[-1][0]} правил: "
              f"CPU мс/успешный запрос ≈ {intercept:.2f} + {slope:.5f} × N; R²={r2:.3f}. "
              "МНК по медианам точек; без экстраполяции за измеренный диапазон.")
    print("\n| Прямой upstream | Прогоны | RPS, медиана (min–max) | Ошибки / таймауты |")
    print("|---|---|---|---|")
    for proto in ("udp", "tcp"):
        cell = [r for r in rows if r["metric"] == "upstream" and r["proto"] == proto]
        if not cell:
            continue
        values = [r["rps"] for r in cell]
        print(f"| {proto} | {len(cell)} | {statistics.median(values):.0f} "
              f"({min(values):.0f}–{max(values):.0f}) | {sum(r['errors'] for r in cell)} / "
              f"{sum(r['timeouts'] for r in cell)} |")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, nargs="+")
    summarize(parser.parse_args().results)
