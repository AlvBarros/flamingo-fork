#!/usr/bin/env python3
"""Compare two runs over the same stretch of the cutscene (it plays in real time),
or sum up one, from the frame log's lines (`VICTORY_FRAME_LOG=1`,
crates/victory-gpu/src/framelog.rs).

    python3 -I scripts/linux/ab_runs.py <log A> [<log B>] [from s] [to s] [min draws]

Times count from each run's first frame-rate line with at least `min draws`
draws a frame (1000); frame-log lines between `from` and `to` seconds after it
(60, 480) are counted, while frames still draw at least `min draws`.

What it reads, every five seconds of a run:
  "frame times: ... each: 150 160 140 400 150"       every frame's length (ms)
  "usual frame (the mean of N not long): 150 ms | ..." the mean make-up of the N
                                                     frames that were not long
  "long frame 3 at 3.0 s: 400 ms | ..."               one long frame's make-up
  "long frames: K more in this reading are not listed"

It prints the frame times (mean, median, 90th and 99th percentile, the share
over 250 ms and 1 s, the time spent in long frames), then each column of the
make-up: its mean in usual frames (weighted by the frames each line stands
for), its median and sum over the long frames listed, and, for two runs, how B
differs from A. A column a log does not have is left out.
"""
import re
import statistics as st
import sys
from datetime import datetime

ansi = re.compile(r"\x1b\[[0-9;]*m")
ts = lambda line: datetime.fromisoformat(line[:26].rstrip("Z"))
N = r"(-?[0-9]+(?:\.[0-9]+)?)"
# The make-up's columns, in the line's order: name, pattern (its first group).
# The memory columns (remapping, xmem, memory watch) decide whether the memory
# bridge pays.
fields = {
    "frame ms": rf"(?:not long\)|s): {N} ms \|",
    "render thread ran ms": rf"render thread: ran {N} ms",
    "present ms": rf"in the present call {N}",
    "present copy+show ms": rf"copy and show {N}",
    "render submit calls ms": rf"in submit calls {N}",
    "render paced ms": rf"paced {N} \|",
    "submissions": rf"submissions: {N},",
    "prepare ms": rf"preparing {N} ms",
    "card wait ms": rf"waiting for the card {N}",
    "copies ms": rf"copies {N} ms",
    "copies MB": rf"copies -?[0-9.]+ ms {N} MB",
    "copy-back ms": rf"copy-back {N} ms",
    "copy-back MB": rf"copy-back -?[0-9.]+ ms {N} MB",
    "card idle ms": rf"card: idle {N} ms",
    "card idle behind ms": rf"\({N} behind us\)",
    "all submit calls ms": rf"all submit calls {N} ms",
    "texture refresh ms": rf"textures: refresh {N} ms",
    "refresh other-thread ms": rf"{N} waiting for another thread's refresh",
    "refresh asking ms": rf"{N} asking the watch",
    "refresh hold-still ms": rf"{N} waiting for memory to hold still",
    "uploads": rf"{N} uploads",
    "remap calls": rf"remapping: {N} calls",
    "remap ms": rf"remapping: -?[0-9.]+ calls {N} ms",
    "remap wait ms": rf"\({N} waiting for readers\)",
    "xmem calls": rf"xmem: {N} mapping calls",
    "xmem ms": rf"mapping calls {N} ms",
    "Windows calls": rf"Windows calls {N} ",
    "Windows ms": rf"Windows calls -?[0-9.]+ {N} ms",
    "GPU side told": rf"the GPU side told {N} ",
    "GPU side told ms": rf"the GPU side told -?[0-9.]+ {N} ms",
    "write-watch askings": rf"{N} write-watch askings",
    "write-watch refused": rf"askings \({N} refused\)",
    "write-watch ms": rf"askings \(-?[0-9.]+ refused\) {N} ms",
    "write-watch MB": rf"ms over {N} MB",
    "protection changes": rf"{N} protection changes",
    "protection ms": rf"protection changes {N} ms",
    "region queries": rf"{N} region queries",
    "region ms": rf"region queries {N} ms",
    "guard waits": rf"waits for the guards {N} ",
    "guard wait ms": rf"waits for the guards -?[0-9.]+ {N} ms",
    "watch locks wait ms": rf"for its other locks {N} ms",
    "guard faults": rf"{N} guard faults",
    "frame threads: watch ms": rf"frame's threads: asking {N} ms",
    "frame threads: protect ms": rf"protecting {N} ms",
    "frame threads: query ms": rf"querying {N} ms",
    "frame threads: guard wait ms": rf"guard-waiting {N} ms",
    "frame threads: lock wait ms": rf"lock-waiting {N} ms",
    "frame threads: guard faults": rf"{N} faults taken",
    "pipeline wait ms": rf"pipelines: {N} ms waited",
    "disk reads": rf"disk: {N} reads",
    "disk MB": rf"reads {N} MB",
    "disk worker ms": rf"MB {N} worker ms",
    "pacing ms": rf"pacing, all threads {N} ms",
    "process user ms": rf"process: {N} user",
    "process kernel ms": rf"user {N} kernel ms",
    "page faults": rf"{N} page faults",
    "draws": rf"\| {N} draws",
}
compiled = {name: re.compile(rx) for name, rx in fields.items()}


def columns(line):
    """The make-up's columns found in a frame-log line."""
    return {name: float(m.group(1)) for name, rx in compiled.items() if (m := rx.search(line))}


def load(log, lo, hi, min_draws):
    draws, times, usual, long, unlisted = [], [], [], [], []
    for raw in open(log, errors="replace"):
        line = ansi.sub("", raw)
        if not line[:4].isdigit():
            continue
        if m := re.search(r"host: [0-9.]+ fps, .* ([0-9]+) draws/frame", line):
            draws.append((ts(line), int(m.group(1))))
        elif "framelog: frame times" in line and (m := re.search(r"each: ([0-9 ]+)", line)):
            times.append((ts(line), [int(x) for x in m.group(1).split()]))
        elif m := re.search(r"framelog: usual frame \(the mean of ([0-9]+) not long\)", line):
            usual.append((ts(line), int(m.group(1)), columns(line)))
        elif "framelog: long frame " in line:
            long.append((ts(line), columns(line)))
        elif m := re.search(r"framelog: long frames: ([0-9]+) more", line):
            unlisted.append((ts(line), int(m.group(1))))
    start = next((t for t, d in draws if d >= min_draws), None)
    if start is None:
        sys.exit(f"{log}: never {min_draws} draws a frame")

    def ok(t):
        s = (t - start).total_seconds()
        before = [d for at, d in draws if at <= t]
        return lo <= s <= hi and before and before[-1] >= min_draws

    return {
        "frames": [f for t, fs in times if ok(t) for f in fs],
        "usual": [(n, c) for t, n, c in usual if ok(t)],
        "long": [c for t, c in long if ok(t)],
        "unlisted": sum(n for t, n in unlisted if ok(t)),
    }


def percentile(values, p):
    v = sorted(values)
    return v[min(len(v) - 1, int(len(v) * p))] if v else 0.0


def summary(run):
    """Frame-time figures of a run, by name."""
    f = run["frames"]
    if not f:
        return {}
    long_ms = sum(c.get("frame ms", 0.0) for c in run["long"])
    return {
        "frames": len(f),
        "frame mean ms": st.mean(f),
        "frame median ms": st.median(f),
        "frame p90 ms": percentile(f, 0.9),
        "frame p99 ms": percentile(f, 0.99),
        "frame max ms": max(f),
        "over 250 ms %": 100 * sum(x > 250 for x in f) / len(f),
        "over 1 s %": 100 * sum(x > 1000 for x in f) / len(f),
        "long frames": len(run["long"]) + run["unlisted"],
        "time in long frames %": 100 * long_ms / sum(f) if sum(f) else 0.0,
    }


def makeup(run):
    """Per column: (mean in usual frames, median over long frames, sum over long frames,
    the long frames' share of the column's total in %)."""
    out = {}
    weight = sum(n for n, _ in run["usual"])
    for name in fields:
        u = [(n, c[name]) for n, c in run["usual"] if name in c]
        lv = [c[name] for c in run["long"] if name in c]
        if not u and not lv:
            continue
        usual = sum(n * x for n, x in u) / weight if u and weight else None
        total = sum(n * x for n, x in u) + sum(lv)
        share = 100 * sum(lv) / total if lv and total else None
        out[name] = (usual, st.median(lv) if lv else None, sum(lv) if lv else None, share)
    return out


def num(x, fmt="{:.1f}"):
    return "" if x is None else fmt.format(x)


def change(x, y):
    return f"{(y - x) / x * 100:+.0f}%" if x not in (None, 0) and y is not None else ""


args = sys.argv[1:]
logs = [a for a in args[:2] if not re.fullmatch(r"[0-9.]+", a)]
rest = args[len(logs):]
if not logs:
    sys.exit(__doc__)
lo = float(rest[0]) if len(rest) > 0 else 60
hi = float(rest[1]) if len(rest) > 1 else 480
md = int(rest[2]) if len(rest) > 2 else 1000
runs = [load(log, lo, hi, md) for log in logs]
for tag, log in zip("AB", logs):
    print(f"{tag} {log}")
print(f"{lo:.0f}-{hi:.0f} s after drawing starts, >= {md} draws/frame\n")

sums = [summary(r) for r in runs]
print(f"{'frame times':26}" + "".join(f"{t:>12}" for t in "AB"[: len(runs)]) + (f"{'B vs A':>12}" if len(runs) == 2 else ""))
for name in sums[0] or (sums[-1] if sums else {}):
    vals = [s.get(name) for s in sums]
    fmt = "{:.0f}" if name in ("frames", "long frames") or name.endswith("ms") else "{:.1f}"
    line = f"{name:26}" + "".join(f"{num(v, fmt):>12}" for v in vals)
    if len(runs) == 2:
        line += f"{change(vals[0], vals[1]):>12}"
    print(line)

made = [makeup(r) for r in runs]
names = [n for n in fields if any(n in m for m in made)]
if names:
    print(
        "\nthe frame's make-up: mean in usual frames | median, sum over the long frames listed, and the long "
        "frames' share of the column's total"
    )
    head = f"{'':26}" + "".join(
        f"{t + ' usual':>11}{t + ' long med':>12}{t + ' long sum':>12}{t + ' long %':>9}" for t in "AB"[: len(runs)]
    )
    if len(runs) == 2:
        head += f"{'usual B/A':>11}{'long B/A':>10}"
    print(head)
    for name in names:
        cells = [m.get(name, (None, None, None, None)) for m in made]
        line = f"{name:26}" + "".join(
            f"{num(u):>11}{num(med):>12}{num(s):>12}{num(p, '{:.0f}'):>9}" for u, med, s, p in cells
        )
        if len(runs) == 2:
            line += f"{change(cells[0][0], cells[1][0]):>11}{change(cells[0][2], cells[1][2]):>10}"
        print(line)
