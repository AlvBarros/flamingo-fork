#!/usr/bin/env python3
"""A/B of a switch that alternates in one run (a log line "... from now (VAR=alt): ON|off").

    python3 -I scripts/linux/ab.py <play log> [min draws per frame, default 1000]

Each frame-log line (frame times; usual/long frame make-up) is given to the phase
its timestamp falls in (3 s after a switch skipped), and counted only while the
frame-rate lines say frames draw at least that many draws (a run whose pipelines
are stuck draws a handful and is not a measurement).
"""
import re, sys, statistics as st
from datetime import datetime

log = sys.argv[1]
min_draws = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
ansi = re.compile(r"\x1b\[[0-9;]*m")
ts = lambda line: datetime.fromisoformat(line[:26].rstrip("Z"))
switches, draws, times, makeup = [], [], [], []
for raw in open(log, errors="replace"):
    line = ansi.sub("", raw)
    if not line[:4].isdigit():
        continue
    if "from now (VICTORY_" in line and "=alt)" in line:
        switches.append((ts(line), line.rstrip().endswith("ON")))
    elif m := re.search(r"host: [0-9.]+ fps, .* ([0-9]+) draws/frame", line):
        draws.append((ts(line), int(m.group(1))))
    elif "framelog: frame times" in line and (m := re.search(r"each: ([0-9 ]+)", line)):
        times.append((ts(line), [int(x) for x in m.group(1).split()]))
    elif re.search(r"framelog: (usual frame|long frame)", line):
        makeup.append((ts(line), line))

def phase(t):
    on = None
    for at, state in switches:
        if at <= t:
            on = state if (t - at).total_seconds() >= 3 else None
    return on

def drawing(t):
    before = [d for at, d in draws if at <= t]
    return bool(before) and before[-1] >= min_draws

fields = {
    "copies ms": r"copies ([0-9.]+) ms", "copies MB": r"copies [0-9.]+ ms ([0-9.]+) MB",
    "copy-back ms": r"copy-back ([0-9.]+) ms", "texture refresh ms": r"textures: refresh ([0-9.]+) ms",
    "write-watch asks": r"([0-9]+) write-watch askings", "write-watch ms": r"askings \([0-9]+ refused\) ([0-9.]+) ms",
    "protection changes": r"([0-9]+) protection changes", "protection ms": r"protection changes ([0-9.]+) ms",
    "region queries": r"([0-9]+) region queries", "region ms": r"region queries ([0-9.]+) ms",
    "remap ms": r"remapping: [0-9]+ calls ([0-9.]+) ms", "hot kept": r"hot set: ([0-9]+) blocks left",
}
out = {True: {"frames": []}, False: {"frames": []}}
for t, frames in times:
    p = phase(t)
    if p is not None and drawing(t):
        out[p]["frames"] += frames
for t, line in makeup:
    p = phase(t)
    if p is None or not drawing(t):
        continue
    for name, rx in fields.items():
        if m := re.search(rx, line):
            out[p].setdefault(name, []).append(float(m.group(1)))
if not switches:
    sys.exit("no alt switch lines in the log")
print(f"{log}: switches {len(switches)}, counted while >= {min_draws} draws/frame")
print(f"{'':22}{'ON':>12}{'off':>12}{'ON vs off':>12}")
def row(name, a, b, fmt="{:.1f}"):
    d = f"{(a - b) / b * 100:+.0f}%" if b else ""
    print(f"{name:22}{fmt.format(a):>12}{fmt.format(b):>12}{d:>12}")
fa, fb = out[True]["frames"], out[False]["frames"]
print(f"{'frames':22}{len(fa):>12}{len(fb):>12}")
if fa and fb:
    row("frame mean ms", st.mean(fa), st.mean(fb), "{:.0f}")
    row("frame median ms", st.median(fa), st.median(fb), "{:.0f}")
for name in fields:
    a, b = out[True].get(name, []), out[False].get(name, [])
    if a and b:
        row(name, st.mean(a), st.mean(b))
