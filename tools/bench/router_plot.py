#!/usr/bin/env python3
"""Regenerate self-contained dark SVG charts for real-router benchmarks.

Reads the original JSONL measurements and plots the median/min–max of three
UDP capacity repeats. Uses only the Python standard library. Never plots a
RAM-guarded (unmeasured) run as a zero.
"""

import argparse
import json
import math
from pathlib import Path
from xml.sax.saxutils import escape


CHARTS = (
    ("rps", "Пропускная способность DNS", "Успешных запросов / с", "benchmark-throughput.svg", 22, 1200, (30, 50, 100, 200, 500, 1000)),
    ("cost", "CPU на один DNS-запрос", "CPU демона, мс / запрос", "benchmark-cpu.svg", .75, 125, (1, 2, 5, 10, 20, 50, 100)),
    ("rss", "Потребление памяти (RSS)", "RSS демона, MiB", "benchmark-memory.svg", 2.4, 32, (3, 5, 10, 20, 30)),
)

BLUE = "#4A90FF"
GOLD = "#FFE18A"
WHITE = "#F7F8FA"
SOFT = "#AEB4BF"
AXIS = "#D8DCE3"
DIM = "#777E89"
LEFT, RIGHT, TOP, BOTTOM = 171.6, 1125.6, 215.76, 633.36


def fmt(number):
    """Stable compact three-decimal SVG coordinates."""
    return f"{number:.3f}".rstrip("0").rstrip(".")


def metric(row, kind):
    if kind == "rps":
        return row["rps"]
    if kind == "rss":
        return row["after"]["VmRSS"] / 1024
    return row["cpu_pct"] / 100 * row["cpu_interval_sec"] / row["ok"] * 1000


def text(x, y, content, css_class="", extra=""):
    return (f'<text x="{fmt(x)}" y="{fmt(y)}" class="{css_class}" {extra}>'
            f'{escape(content)}</text>')


def render_chart(rows, failures, kind, title, ylabel, lo, hi, ticks):
    x = lambda n: LEFT + math.log(n / 85) / math.log(130000 / 85) * (RIGHT - LEFT)
    y = lambda n: BOTTOM - math.log(n / lo) / math.log(hi / lo) * (BOTTOM - TOP)
    output = []
    add = output.append

    add('<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="870" '
        'viewBox="0 0 1200 870" role="img" aria-labelledby="chart-title chart-desc">')
    add(f'<title id="chart-title">{escape(title)} — mt-c против MagiTrickle</title>')
    add('<desc id="chart-desc">Производительность на MT7621, UDP, 10 клиентов. '
        'Логарифмические шкалы. Медиана и min–max трёх запусков по 20 секунд. '
        'Провалы по памяти при 50 и 100 тысячах правил не являются нулевыми значениями.</desc>')
    add(f'''<style>
.title {{ font: 700 34px Inter, Arial, sans-serif; fill: {WHITE} }}
.sub {{ font: 16px Inter, Arial, sans-serif; fill: {SOFT} }}
.legend {{ font: 18px Inter, Arial, sans-serif; fill: {WHITE} }}
.tick {{ font: 16px Inter, Arial, sans-serif; fill: {AXIS} }}
.axis-label {{ font: 17px Inter, Arial, sans-serif; fill: {WHITE} }}
.foot {{ font: italic 15px Inter, Arial, sans-serif; fill: {WHITE} }}
.note {{ font: 14px Inter, Arial, sans-serif; fill: {SOFT} }}
.tiny {{ font: 12px Inter, Arial, sans-serif; fill: {SOFT} }}
.line {{ stroke-linejoin: round; stroke-linecap: round; fill: none; stroke-width: 3.5 }}
</style>''')
    add('<rect width="1200" height="870" fill="#080A0D"/><rect x="18" y="17" '
        'width="1164" height="837" rx="28" stroke="#393D45" '
        'stroke-width="1.45" fill="none"/>')
    add(text(66, 78, title, "title"))
    add(text(66, 121, "MT7621 · UDP · 10 клиентов · реальные DNS-ответы и ipset", "sub"))
    add(text(1130, 78, "mt-c / bench", "note", 'text-anchor="end"'))
    add(f'<path d="M 66 164 H 108" stroke="{BLUE}" stroke-width="3" stroke-linecap="round"/>')
    add(f'<path d="M 87 156 L 95 164 L 87 172 L 79 164 Z" fill="{BLUE}" '
        'stroke="#080A0D" stroke-width="1"/>')
    add(text(122, 170, "mt-c · C", "legend"))
    add(f'<path d="M 432 164 H 474" stroke="{GOLD}" stroke-width="3" stroke-linecap="round"/>')
    add(f'<circle cx="453" cy="164" r="7" fill="{GOLD}" stroke="#080A0D" stroke-width="1"/>')
    add(text(488, 170, "MagiTrickle · Go 0.8.2", "legend"))
    add(f'<path d="M {fmt(LEFT)} {fmt(TOP)} V {fmt(BOTTOM)} H {fmt(RIGHT)}" '
        f'stroke="{AXIS}" stroke-width="1.5" fill="none"/>')
    for value in ticks:
        yy = y(value)
        add(f'<path d="M {fmt(LEFT-9)} {fmt(yy)} H {fmt(LEFT)}" '
            f'stroke="{AXIS}" stroke-width="1.4"/>')
        add(text(LEFT-20, yy+5, str(value), "tick", 'text-anchor="end"'))
    for value, label in ((100, "100"), (1000, "1k"), (10000, "10k"), (100000, "100k")):
        xx = x(value)
        add(f'<path d="M {fmt(xx)} {fmt(BOTTOM)} V {fmt(BOTTOM+9)}" '
            f'stroke="{AXIS}" stroke-width="1.4"/>')
        add(text(xx, BOTTOM+38, label, "tick", 'text-anchor="middle"'))
    add(text((LEFT+RIGHT)/2, BOTTOM+86,
             "Число включённых namespace-правил · log", "axis-label",
             'text-anchor="middle"'))
    add(text(79, (TOP+BOTTOM)/2, f"{ylabel} · log", "axis-label",
             f'text-anchor="middle" transform="rotate(-90 79 {fmt((TOP+BOTTOM)/2)})"'))

    for backend in ("original", "mt-c"):
        color = GOLD if backend == "original" else BLUE
        points = []
        sizes = sorted({r["rules"] for r in rows if r["backend"] == backend})
        for size in sizes:
            trials = [r for r in rows if r["backend"] == backend and r["rules"] == size]
            if len(trials) != 3:
                raise ValueError(f"expected three repeats for {backend}, {size}")
            values = sorted(metric(r, kind) for r in trials)
            points.append((size, values[0], values[1], values[2]))

        for size, minimum, _, maximum in points:
            xx, yy1, yy2 = fmt(x(size)), fmt(y(minimum)), fmt(y(maximum))
            xx0, xx1 = fmt(x(size)-4), fmt(x(size)+4)
            add(f'<g stroke="{color}" stroke-width="1.5" opacity="0.69">'
                f'<path d="M {xx} {yy1} V {yy2}"/>'
                f'<path d="M {xx0} {yy1} H {xx1} M {xx0} {yy2} H {xx1}"/></g>')
        path = " ".join(("M" if i == 0 else "L") + f" {fmt(x(size))} {fmt(y(mid))}"
                        for i, (size, _, mid, _) in enumerate(points))
        add(f'<path d="{path}" stroke="{color}" class="line"/>')
        for size, _, median, _ in points:
            xx, yy = x(size), y(median)
            if backend == "mt-c":
                add(f'<path d="M {fmt(xx)} {fmt(yy-6)} L {fmt(xx+6)} {fmt(yy)} '
                    f'L {fmt(xx)} {fmt(yy+6)} L {fmt(xx-6)} {fmt(yy)} Z" '
                    f'fill="{color}" stroke="#080A0D" stroke-width="1.25"/>')
            else:
                add(f'<circle cx="{fmt(xx)}" cy="{fmt(yy)}" r="5" '
                    f'fill="{color}" stroke="#080A0D" stroke-width="1.25"/>')

    failed_sizes = sorted({r["rules"] for r in failures})
    for size in failed_sizes:
        xx, yy = x(size), BOTTOM-23
        add(f'<path d="M {fmt(xx-5)} {fmt(yy-5)} l 10 10 m -10 0 l 10 -10" '
            f'stroke="{DIM}" stroke-width="2.0" fill="none"/>')
        add(text(xx, yy-16, "RAM", "tiny", 'text-anchor="middle"'))
    add(text(66, 776, "Медиана и min–max · 3 × 20 с · генератор на роутере · 14.10.2026", "foot"))
    if failed_sizes:
        add(text(66, 816, "× 50k / 100k: оба запуска остановлены резервом памяти; "
                 "значение метрики не измерено", "note"))
    add("</svg>")
    return "\n".join(output) + "\n"


def plot(sources, output):
    rows = [json.loads(line) for path in sources
            for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    failures = [r for r in rows if r["metric"] == "failure"
                and r["scenario"] == "capacity" and r["proto"] == "udp"]
    loads = [r for r in rows if r["metric"] == "load"
             and r["scenario"] == "capacity" and r["proto"] == "udp"]
    sizes = sorted({r["rules"] for r in loads + failures})
    for backend in ("original", "mt-c"):
        for size in sizes:
            count = sum(r["backend"] == backend and r["rules"] == size for r in loads)
            stopped = any(r["backend"] == backend and r["rules"] == size for r in failures)
            if count != 3 and not (count == 0 and stopped):
                raise ValueError(f"missing/incomplete samples for {backend}, {size}: {count}")
    output.mkdir(parents=True, exist_ok=True)
    for kind, title, ylabel, filename, minimum, maximum, ticks in CHARTS:
        (output / filename).write_text(
            render_chart(loads, failures, kind, title, ylabel, minimum, maximum, ticks),
            encoding="utf-8",
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, default=Path("img"))
    args = parser.parse_args()
    plot(args.results, args.output)
