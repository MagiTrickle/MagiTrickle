#!/usr/bin/env python3
"""Regenerate README router charts; requires matplotlib, not needed for runs."""
import argparse
import json
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter


def plot(results, output):
    paths = [results] if isinstance(results, Path) else results
    rows = [json.loads(line) for path in paths for line in path.read_text().splitlines() if line]
    failures = [r for r in rows if r["metric"] == "failure" and r["scenario"] == "capacity" and r["proto"] == "udp"]
    rows = [r for r in rows if r["metric"] == "load" and r["scenario"] == "capacity" and r["proto"] == "udp"]
    sizes = sorted({r["rules"] for r in rows + failures})
    plt.rcParams.update({"font.family": "DejaVu Sans", "svg.fonttype": "none",
                         "svg.hashsalt": "mt-c-router-benchmark", "font.size": 12})
    for kind, title, ylabel, filename in [
        ("rps", "DNS на реальном роутере MT7621", "Успешные запросы / с", "benchmark-throughput.svg"),
        ("cost", "CPU на один успешный DNS-запрос", "CPU демона, мс / запрос", "benchmark-cpu.svg"),
        ("rss", "Память после DNS-нагрузки", "RSS демона, MiB", "benchmark-memory.svg"),
    ]:
        fig, ax = plt.subplots(figsize=(12, 6.8), layout="constrained")
        for backend, label, color in [("original", "MagiTrickle 0.8.2 (пакет 0.8.2-2)", "#dc8237"),
                                      ("mt-c", "mt-c · 65c7b01", "#7757d6")]:
            values = []
            failed_sizes = []
            for size in sizes:
                cell = [r for r in rows if r["backend"] == backend and r["rules"] == size]
                if not cell and any(r["backend"] == backend and r["rules"] == size for r in failures):
                    values.append([float("nan")])
                    failed_sizes.append(size)
                    continue
                if len(cell) != 3:
                    raise ValueError(f"expected three repeats for {backend}, {size}")
                values.append([r["rps"] if kind == "rps" else r["after"]["VmRSS"] / 1024 if kind == "rss" else
                               r["cpu_pct"] / 100 * r["cpu_interval_sec"] / r["ok"] * 1000
                               for r in cell])
            medians = [statistics.median(v) for v in values]
            error = [[m-min(v) for m, v in zip(medians, values)],
                     [max(v)-m for m, v in zip(medians, values)]]
            ax.errorbar(sizes, medians, yerr=error, color=color, marker="o", lw=2.5,
                        capsize=6, label=label)
            for x, y in zip(sizes, medians):
                if y != y:
                    continue
                ax.annotate(f"{y:.0f}" if kind == "rps" else f"{y:.1f}", (x, y),
                            xytext=(0, 12), textcoords="offset points", ha="center", color=color)
            for x in failed_sizes:
                position = .02 if backend == "original" else .08
                ax.scatter([x], [position], color=color, marker="x", s=60,
                           transform=ax.get_xaxis_transform(), clip_on=False)
                ax.annotate("Go: RAM" if backend == "original" else "C: RAM", (x, position),
                            xycoords=("data", "axes fraction"), xytext=(-5, 4),
                            textcoords="offset points", ha="right", fontsize=9, color=color)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xticks(sizes)
        ax.xaxis.set_major_formatter(ScalarFormatter())
        ax.yaxis.set_major_formatter(ScalarFormatter())
        ax.margins(y=.2)
        ax.set_xlabel("Включённые namespace-правила · логарифмическая шкала")
        ax.set_ylabel(ylabel + " · логарифмическая шкала")
        ax.set_title(title + "\nUDP · 10 клиентов · реальные DNS-ответы и ipset", pad=20)
        ax.grid(axis="y", alpha=.25)
        ax.spines[["top", "right"]].set_visible(False)
        ax.legend(loc="best", frameon=False)
        footer = "Медиана и min–max 3 × 20 с · генератор на роутере · 14.10.2026"
        if failures:
            footer += "\n× RAM — запуск остановлен резервом памяти; значение метрики не измерено"
        fig.supxlabel(footer, fontsize=10)
        destination = output / filename
        fig.savefig(destination, metadata={"Date": None})
        # Matplotlib emits spaces before path-data newlines; keep generated
        # artifacts clean under git diff --check without changing geometry.
        destination.write_text("\n".join(line.rstrip() for line in destination.read_text().splitlines()) + "\n")
        plt.close(fig)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, default=Path("img"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    plot(args.results, args.output)
