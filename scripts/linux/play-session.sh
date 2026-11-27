#!/bin/bash
# A hand-played session with everything recorded, and a report at the end.
#
#   scripts/linux/play-session.sh [minutes]       # hard limit, default 15
#   VICTORY_BRIDGE=0 scripts/linux/play-session.sh 5   # any setting, as run.sh
#
# Settings come as for run.sh (play.env, then play.local.env, then the
# environment), except that the scripted key presses (FORK_KEYS) are off: you
# play. Alongside the game it records the GPU (nvidia-smi, once a second) and
# the system (vmstat: swap, disk wait), in logs/session-<time>/.
#
# While it runs, type a note and press Enter ("prison starts", "big stutter",
# "Lucia has no head"): it is stamped with the time and shown beside the long
# frames in the report. Type q and Enter to end early. At the limit the game
# is stopped (only this prefix's Wine).
#
# The report (also saved as report.txt): whether the memory bridge loaded,
# crashes, frame times per minute, the longest frames with what they were made
# of and the note before them, the frames after each note, and GPU and memory
# peaks. Needs VICTORY_FRAME_LOG=1 (play.local.env sets it).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
MIN=${1:-15}
case "$MIN" in '' | *[!0-9]*) echo "minutes: a whole number, not '$MIN'" >&2; exit 2 ;; esac

if pgrep -f 'victory\.exe' >/dev/null; then
    echo "The game is already running (another session?): not starting a second one." >&2
    exit 1
fi

# The run's home and Wine, as run.sh will have them.
FORK_HOME=$(FORK_KEYS= scripts/linux/run.sh settings 2>/dev/null | sed -n 's/^fork_home *//p')
export FORK_HOME
. scripts/linux/env.sh
WINESERVER="$(dirname "$WINE")/wineserver"

S="logs/session-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$S"
: > "$S/notes.txt"
nvidia-smi --query-gpu=timestamp,utilization.gpu,utilization.memory,memory.used,memory.total,power.draw,clocks.sm,temperature.gpu \
    --format=csv,nounits -l 1 > "$S/gpu.csv" 2>/dev/null &
GPU=$!
vmstat -w -t 1 > "$S/vmstat.txt" 2>/dev/null &
VM=$!

before=$(ls -t logs/*play.log 2>/dev/null | head -1)
FORK_KEYS= scripts/linux/run.sh play > "$S/run.out" 2>&1 &
LOG=""
for _ in $(seq 60); do
    LOG=$(ls -t logs/*play.log 2>/dev/null | head -1)
    [ -n "$LOG" ] && [ "$LOG" != "$before" ] && break
    LOG=""
    sleep 1
done
if [ -z "$LOG" ]; then
    echo "The game did not start a log in 60 s; see $S/run.out" >&2
    kill "$GPU" "$VM" 2>/dev/null
    exit 1
fi
echo "$LOG" > "$S/log.txt"
grep -m1 -E '^ *bridge ' "$LOG" | sed 's/^ */bridge: /' || true

start=$(date +%s)
end=$((start + MIN * 60))
echo
echo "Playing: up to $MIN min (until $(date -d "@$end" +%H:%M:%S)). Log: $LOG"
echo "Type a note + Enter to mark a moment; q + Enter ends the session."
reason="time limit"
while [ "$(date +%s)" -lt "$end" ]; do
    if [ $(( $(date +%s) - start )) -gt 45 ] && ! pgrep -f 'victory\.exe' >/dev/null; then
        reason="the game ended"
        break
    fi
    if read -r -t 5 note; then
        case "$note" in
            q | Q) reason="ended by you"; break ;;
            "") ;;
            *) echo "$(date +%s) $(date +%H:%M:%S) $note" >> "$S/notes.txt"
               echo "  noted at $(date +%H:%M:%S)" ;;
        esac
    fi
done

echo "Session over ($reason): stopping the game."
WINEPREFIX="$FORK_HOME/prefix/pfx" "$WINESERVER" -k 2>/dev/null
sleep 3
kill "$GPU" "$VM" 2>/dev/null
wait 2>/dev/null
python3 -I scripts/linux/session_report.py "$S" "$LOG" | tee "$S/report.txt"
echo "Everything is in $S (report.txt, notes.txt, gpu.csv, vmstat.txt) and $LOG"
