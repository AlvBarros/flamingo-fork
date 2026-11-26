#!/usr/bin/env python3
"""Compare two runs over the same stretch of the cutscene (it plays in real time).

    python3 -I scripts/linux/ab_runs.py <log A> <log B> [from s] [to s] [min draws]

Times count from each run's first frame-rate line with at least `min draws`
draws a frame (1000); frame-log lines between `from` and `to` seconds after it
(60, 480) are counted, while frames still draw at least `min draws`.
"""
import re, sys, statistics as st
from datetime import datetime

ansi = re.compile(r"\x1b\[[0-9;]*m")
ts = lambda line: datetime.fromisoformat(line[:26].rstrip("Z"))
fields = {
    "copies ms": r"copies ([0-9.]+) ms", "copies MB": r"copies [0-9.]+ ms ([0-9.]+) MB",
    "copy-back ms": r"copy-back ([0-9.]+) ms", "texture refresh ms": r"textures: refresh ([0-9.]+) ms",
    "write-watch ms": r"askings \([0-9]+ refused\) ([0-9.]+) ms", "protection ms": r"protection changes ([0-9.]+) ms",
    "region ms": r"region queries ([0-9.]+) ms", "remap ms": r"remapping: [0-9]+ calls ([0-9.]+) ms",
}

def load(log, lo, hi, min_draws):
    draws, times, makeup = [], [], []
    for raw in open(log, errors="replace"):
        line = ansi.sub("", raw)
        if not line[:4].isdigit():
            continue
        if m := re.search(r"host: [0-9.]+ fps, .* ([0-9]+) draws/frame", line):
            draws.append((ts(line), int(m.group(1))))
        elif "framelog: frame times" in line and (m := re.search(r"each: ([0-9 ]+)", line)):
            times.append((ts(line), [int(x) for x in m.group(1).split()]))
        elif re.search(r"framelog: (usual frame|long frame)", line):
            makeup.append((ts(line), line))
    start = next((t for t, d in draws if d >= min_draws), None)
    if start is None:
        sys.exit(f"{log}: never {min_draws} draws a frame")
    def ok(t):
        s = (t - start).total_seconds()
        before = [d for at, d in draws if at <= t]
        return lo <= s <= hi and before and before[-1] >= min_draws
    frames = [f for t, fs in times if ok(t) for f in fs]
    out = {"frames": frames}
    for t, line in makeup:
        if ok(t):
            for name, rx in fields.items():
                if m := re.search(rx, line):
                    out.setdefault(name, []).append(float(m.group(1)))
    return out

a_log, b_log = sys.argv[1], sys.argv[2]
lo = float(sys.argv[3]) if len(sys.argv) > 3 else 60
hi = float(sys.argv[4]) if len(sys.argv) > 4 else 480
md = int(sys.argv[5]) if len(sys.argv) > 5 else 1000
a, b = load(a_log, lo, hi, md), load(b_log, lo, hi, md)
print(f"A {a_log}\nB {b_log}\n{lo:.0f}-{hi:.0f} s after drawing starts, >= {md} draws/frame")
print(f"{'':22}{'A':>12}{'B':>12}{'B vs A':>12}")
def row(name, x, y, fmt="{:.1f}"):
    d = f"{(y - x) / x * 100:+.0f}%" if x else ""
    print(f"{name:22}{fmt.format(x):>12}{fmt.format(y):>12}{d:>12}")
print(f"{'frames':22}{len(a['frames']):>12}{len(b['frames']):>12}")
if a["frames"] and b["frames"]:
    row("frame mean ms", st.mean(a["frames"]), st.mean(b["frames"]), "{:.0f}")
    row("frame median ms", st.median(a["frames"]), st.median(b["frames"]), "{:.0f}")
for name in fields:
    if a.get(name) and b.get(name):
        row(name, st.mean(a[name]), st.mean(b[name]))
