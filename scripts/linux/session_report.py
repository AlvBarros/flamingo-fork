#!/usr/bin/env python3
"""The report of a hand-played session (scripts/linux/play-session.sh).

    python3 -I scripts/linux/session_report.py <session dir> <play log>

Reads the game's log (the frame log: VICTORY_FRAME_LOG=1), the notes typed
during the session, and the GPU and vmstat recordings. Times are local.
"""
import csv, re, statistics as st, sys
from datetime import datetime, timezone

ansi = re.compile(r"\x1b\[[0-9;]*m")
N = r"(-?[0-9.]+)"
# What a long frame was made of: the columns worth naming (summed over
# threads where the frame log says "all threads", so they can exceed the frame).
PARTS = {
    "copies": rf"copies {N} ms",
    "remapping": rf"remapping: [0-9.]+ calls {N} ms",
    "region queries (all threads)": rf"region queries {N} ms",
    "guard waits (all threads)": rf"waits for the guards [0-9.]+ {N} ms",
    "protections (all threads)": rf"protection changes {N} ms",
    "write watch (all threads)": rf"askings \(-?[0-9.]+ refused\) {N} ms",
    "region queries (frame's threads)": rf"querying {N} ms",
    "guard waits (frame's threads)": rf"guard-waiting {N} ms",
    "pipelines waited": rf"pipelines: {N} ms waited",
    "disk": rf"MB {N} worker ms",
    "card idle": rf"card: idle {N} ms",
}


def when(line):
    """The log's UTC stamp as local time (seconds since the epoch)."""
    try:
        t = datetime.fromisoformat(line[:26].rstrip("Z")).replace(tzinfo=timezone.utc)
    except ValueError:
        return None
    return t.timestamp()


def hm(t):
    return datetime.fromtimestamp(t).strftime("%H:%M:%S")


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))] if v else 0


def read_log(path):
    frames, longs, bridge, crashes = [], [], [], []
    for raw in open(path, errors="replace"):
        line = ansi.sub("", raw)
        if "memory bridge" in line:
            bridge.append(line.strip()[:200])
        if "unhandled exception" in line:
            crashes.append(line.strip()[:200])
        if "framelog: frame times" in line and (m := re.search(r"each: ([-0-9 ]+)", line)):
            t = when(line)
            each = [float(x) for x in m.group(1).split()]
            # The line closes ~5 s of frames: spread their times backwards.
            at = t
            for ms in reversed(each):
                frames.append((at, ms))
                at -= ms / 1000
        elif "framelog: long frame" in line and (m := re.search(r"long frame [0-9]+ at [0-9.]+ s: ([0-9.]+) ms \| (.*)", line)):
            parts = {}
            for name, p in PARTS.items():
                if (v := re.search(p, m.group(2))):
                    parts[name] = float(v.group(1))
            longs.append((when(line), float(m.group(1)), parts))
    frames.sort()
    return frames, longs, bridge, crashes


def read_notes(path):
    notes = []
    try:
        for line in open(path):
            epoch, _, text = line.rstrip("\n").split(" ", 2)
            notes.append((float(epoch), text))
    except (OSError, ValueError):
        pass
    return notes


def note_before(notes, t):
    best = None
    for at, text in notes:
        if at <= t:
            best = (at, text)
    return f'{int(t - best[0])} s after "{best[1]}"' if best else ""


def main():
    session, log = sys.argv[1], sys.argv[2]
    frames, longs, bridge, crashes = read_log(log)
    notes = read_notes(f"{session}/notes.txt")
    print("\n===== session report =====")
    print("memory bridge:", bridge[0].split("  ")[-1] if bridge else "not asked (VICTORY_BRIDGE unset or 0)")
    print("crashes:", len(crashes) and crashes[0] or "none")
    if not frames:
        print("no frame-log lines: is VICTORY_FRAME_LOG=1 set (play.local.env)?")
        return
    ms = [f for _, f in frames if f > 0]
    print(f"\nframes: {len(ms)}, median {st.median(ms):.0f} ms ({1000 / st.median(ms):.0f} fps), "
          f"p99 {pct(ms, .99):.0f} ms, longest {max(ms):.0f} ms, "
          f"over 250 ms: {sum(f > 250 for f in ms)}, over 1 s: {sum(f > 1000 for f in ms)}")

    print("\nper minute         frames  median   p99  longest  >250ms  note in that minute")
    t0 = frames[0][0]
    minute = {}
    for t, f in frames:
        minute.setdefault(int((t - t0) // 60), []).append((t, f))
    for k in sorted(minute):
        v = [f for _, f in minute[k] if f > 0]
        if not v:
            continue
        a, b = minute[k][0][0], minute[k][-1][0]
        said = "; ".join(text for at, text in notes if a <= at < a + 60)
        print(f"{hm(a)}-{hm(b)[3:]}  {len(v):6d}  {st.median(v):6.0f} {pct(v, .99):5.0f} {max(v):8.0f} "
              f"{sum(f > 250 for f in v):7d}  {said}")

    if longs:
        print("\nthe longest frames, and the three largest parts of each:")
        for t, f, parts in sorted(longs, key=lambda x: -x[1])[:12]:
            top = sorted(parts.items(), key=lambda kv: -kv[1])[:3]
            made = ", ".join(f"{name} {v:.0f}" for name, v in top)
            print(f"  {hm(t)}  {f:6.0f} ms  {made}  {note_before(notes, t)}")
        sums = {name: sum(p.get(name, 0) for _, _, p in longs) for name in PARTS}
        print(f"\nover all {len(longs)} long frames listed, summed (ms): "
              + ", ".join(f"{k} {v:.0f}" for k, v in sorted(sums.items(), key=lambda kv: -kv[1])[:6]))

    if notes:
        print("\nthe 30 s after each note:")
        for at, text in notes:
            v = [f for t, f in frames if at <= t < at + 30 and f > 0]
            if v:
                print(f"  {hm(at)} {text!r}: {len(v)} frames, median {st.median(v):.0f} ms, longest {max(v):.0f}, over 250 ms {sum(f > 250 for f in v)}")
            else:
                print(f"  {hm(at)} {text!r}: no frames logged")

    try:
        rows = list(csv.reader(open(f"{session}/gpu.csv")))[1:]
        util = [float(r[1]) for r in rows]
        used = [float(r[3]) for r in rows]
        power = [float(r[5]) for r in rows]
        total = float(rows[0][4])
        print(f"\nGPU: load median {st.median(util):.0f}% (max {max(util):.0f}%), VRAM peak {max(used):.0f} of {total:.0f} MiB, "
              f"power max {max(power):.0f} W")
    except (OSError, ValueError, IndexError, st.StatisticsError):
        print("\nGPU: no recording")
    try:
        lines = [l.split() for l in open(f"{session}/vmstat.txt") if l.strip()[:1].isdigit()]
        si = [int(l[6]) for l in lines]
        so = [int(l[7]) for l in lines]
        wa = [int(l[15]) for l in lines]
        free = [int(l[3]) for l in lines]
        print(f"system: free memory min {min(free) // 1024} MiB, swap in/out max {max(si)}/{max(so)} KiB/s, "
              f"disk wait max {max(wa)}%")
    except (OSError, ValueError, IndexError):
        print("system: no recording")


if __name__ == "__main__":
    main()
