#!/usr/bin/env python3
"""Render the real-router README benchmarks as self-contained, dark SVG charts.

Requires matplotlib to regenerate the published assets; benchmark execution
itself remains dependency-free. Source data and median/min-max semantics are
unchanged by visual presentation.
"""

import argparse
import json
import math
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import FancyBboxPatch
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator


# OpenAI-inspired editorial chart treatment. Do not use the OpenAI wordmark:
# these benchmarks are an independent comparison of MagiTrickle and mt-c.
BACKGROUND = "#080A0D"
BORDER = "#393D45"
TEXT = "#F7F8FA"
SECONDARY = "#AEB4BF"
AXIS = "#D8DCE3"
FAINT = "#777E89"
PALETTE = {
    "mt-c": ("mt-c · C", "#4A90FF", "D"),
    "original": ("MagiTrickle · Go 0.8.2", "#FFE18A", "o"),
}
CHARTS = {
    "rps": {
        "title": "Пропускная способность DNS",
        "ylabel": "Успешных запросов / с",
        "filename": "benchmark-throughput.svg",
        "limits": (22, 1200),
        "ticks": (30, 50, 100, 200, 500, 1000),
    },
    "cost": {
        "title": "CPU на один DNS-запрос",
        "ylabel": "CPU демона, мс / запрос",
        "filename": "benchmark-cpu.svg",
        "limits": (0.75, 125),
        "ticks": (1, 2, 5, 10, 20, 50, 100),
    },
    "rss": {
        "title": "Потребление памяти (RSS)",
        "ylabel": "RSS демона, MiB",
        "filename": "benchmark-memory.svg",
        "limits": (2.4, 32),
        "ticks": (3, 5, 10, 20, 30),
    },
}


def metric_value(row, kind):
    if kind == "rps":
        return row["rps"]
    if kind == "rss":
        return row["after"]["VmRSS"] / 1024
    return row["cpu_pct"] / 100 * row["cpu_interval_sec"] / row["ok"] * 1000


def add_header(fig, title):
    # Rounded card border, same for all three charts; black plot interior.
    fig.patches.append(FancyBboxPatch(
        (0.015, 0.018), 0.97, 0.963,
        transform=fig.transFigure,
        boxstyle="round,pad=0.002,rounding_size=0.03",
        linewidth=1.05, edgecolor=BORDER, facecolor="none",
        clip_on=False, zorder=20,
    ))
    fig.text(0.055, 0.925, title, color=TEXT, fontsize=25, fontweight="bold",
             ha="left", va="center")
    fig.text(0.055, 0.868, "MT7621 · UDP · 10 клиентов · реальные DNS-ответы и ipset",
             color=SECONDARY, fontsize=12.2, ha="left", va="center")
    fig.text(0.942, 0.924, "mt-c / bench", color=SECONDARY,
             fontsize=11, ha="right", va="center", fontweight="medium")

    for x, backend in [(0.058, "mt-c"), (0.36, "original")]:
        label, color, marker = PALETTE[backend]
        fig.lines.append(Line2D([x, x + .035], [.811, .811],
                                color=color, linewidth=2.25,
                                solid_capstyle="round", transform=fig.transFigure))
        fig.lines.append(Line2D([x + .0175], [.811], color=color,
                                marker=marker, markersize=8, linestyle="None",
                                markeredgewidth=0, transform=fig.transFigure))
        fig.text(x + .047, .811, label, color=TEXT, fontsize=13.4,
                 ha="left", va="center")


def setup_axes(fig, config):
    ax = fig.add_axes((0.143, 0.272, 0.795, 0.480), facecolor="none")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(85, 130000)
    ax.set_ylim(*config["limits"])
    ax.xaxis.set_major_locator(FixedLocator([100, 1000, 10000, 100000]))
    ax.xaxis.set_major_formatter(FuncFormatter(
        lambda value, _: {100: "100", 1000: "1k", 10000: "10k", 100000: "100k"}.get(value, "")))
    ax.yaxis.set_major_locator(FixedLocator(config["ticks"]))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_minor_locator(NullLocator())
    for side in ["top", "right"]:
        ax.spines[side].set_visible(False)
    for side in ["left", "bottom"]:
        ax.spines[side].set_color(AXIS)
        ax.spines[side].set_linewidth(1.15)
    ax.tick_params(axis="both", which="major", length=6, width=1.15,
                   color=AXIS, labelcolor=AXIS, labelsize=11.5, pad=11)
    ax.set_xlabel("Число включённых namespace-правил · log",
                  color=TEXT, fontsize=12.5, labelpad=22)
    ax.set_ylabel(config["ylabel"] + " · log", color=TEXT,
                  fontsize=12.5, labelpad=25)
    ax.grid(False)
    ax.set_axisbelow(True)
    return ax


def add_footer(fig, has_memory_failures):
    fig.text(0.055, 0.108,
             "Медиана и min–max · 3 × 20 с · генератор на роутере · 14.10.2026",
             color=TEXT, fontsize=11.1, fontstyle="italic", ha="left")
    if has_memory_failures:
        fig.text(0.055, 0.067,
                 "× 50k / 100k: оба запуска остановлены резервом памяти; значение метрики не измерено",
                 color=SECONDARY, fontsize=10.5, ha="left")


def plot(results, output):
    paths = [results] if isinstance(results, Path) else results
    rows = [json.loads(line) for path in paths
            for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    failures = [r for r in rows if r["metric"] == "failure"
                and r["scenario"] == "capacity" and r["proto"] == "udp"]
    rows = [r for r in rows if r["metric"] == "load"
            and r["scenario"] == "capacity" and r["proto"] == "udp"]
    sizes = sorted({r["rules"] for r in rows + failures})
    plt.rcParams.update({
        "font.family": ["Inter", "DejaVu Sans", "sans-serif"],
        "svg.fonttype": "none", "svg.hashsalt": "mt-c-router-benchmark",
        "font.size": 12,
    })

    for kind, cfg in CHARTS.items():
        fig = plt.figure(figsize=(12, 8.7), dpi=100, facecolor=BACKGROUND)
        add_header(fig, cfg["title"])
        ax = setup_axes(fig, cfg)

        for backend in ("original", "mt-c"):
            _, color, marker = PALETTE[backend]
            xs, medians, lo, hi = [], [], [], []
            for size in sizes:
                cell = [r for r in rows if r["backend"] == backend and r["rules"] == size]
                if not cell:
                    if any(r["backend"] == backend and r["rules"] == size for r in failures):
                        continue  # RAM guard is not a measured zero.
                    raise ValueError(f"missing benchmark results: {backend}, {size}")
                if len(cell) != 3:
                    raise ValueError(f"expected three repeats for {backend}, {size}")
                values = [metric_value(r, kind) for r in cell]
                xs.append(size)
                medians.append(statistics.median(values))
                lo.append(min(values))
                hi.append(max(values))

            # Smaller, semi-transparent uncertainty whiskers don't compete with
            # the prominent median trajectory; the values are still min–max.
            ax.errorbar(xs, medians,
                        yerr=[[m - a for m, a in zip(medians, lo)],
                              [b - m for m, b in zip(medians, hi)]],
                        color=color, marker=marker, markersize=6.7,
                        markeredgecolor=BACKGROUND, markeredgewidth=1.0,
                        linewidth=2.7, capsize=3.2, capthick=1.25,
                        elinewidth=1.1, solid_capstyle="round", zorder=5)

        if failures:
            # Compact crosses near the baseline, clearly labeled as RAM guard,
            # never joined to a performance line or plotted as zero.
            affected = sorted({f["rules"] for f in failures})
            for size in affected:
                ax.plot(size, .055, marker="x", markersize=8,
                        markeredgewidth=1.5, color=FAINT, linestyle="None",
                        transform=ax.get_xaxis_transform(), clip_on=False)
                ax.annotate("RAM", xy=(size, .055), xycoords=ax.get_xaxis_transform(),
                            xytext=(0, 10), textcoords="offset points", ha="center",
                            fontsize=8.6, color=SECONDARY)

        add_footer(fig, bool(failures))
        destination = output / cfg["filename"]
        fig.savefig(destination, format="svg", facecolor=BACKGROUND,
                    metadata={"Date": None})
        # Matplotlib sometimes emits trailing spaces before line breaks.
        destination.write_text("\n".join(line.rstrip() for line in
                                    destination.read_text(encoding="utf-8").splitlines()) + "\n",
                               encoding="utf-8")
        plt.close(fig)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, default=Path("img"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    plot(args.results, args.output)
