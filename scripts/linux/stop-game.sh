#!/bin/bash
# Stops the game the safe way: asks it to close (as its window's close button
# does), so it suspends the title and lets the card's work run out before the
# process ends; only if it is still there after a while is it killed.
#
#   scripts/linux/stop-game.sh            # FORK_HOME and Wine as run.sh has them
#   FORK_STOP_WAIT=60 scripts/linux/stop-game.sh
#
# Why: on 2026-11-27 a run ended with `wineserver -k` while the card was busy
# faulted the card (Xid 31) and NVIDIA's driver hung releasing the dead
# process's memory: black screen, no keyboard or mouse, a reset needed
# (docs/memory-bridge-plan.md §11). Never stop the game with `wineserver -k`
# or `kill -9` first; use this.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
WAIT=${FORK_STOP_WAIT:-30}

running() { pgrep -f 'victory\.exe' >/dev/null; }

if ! running; then
    echo "stop-game: the game is not running."
    exit 0
fi

if [ -z "${FORK_HOME:-}" ]; then
    FORK_HOME=$(FORK_KEYS= scripts/linux/run.sh settings 2>/dev/null | sed -n 's/^fork_home *//p')
fi
export FORK_HOME
. scripts/linux/env.sh
WINESERVER="$(dirname "$WINE")/wineserver"
export WINEPREFIX="$FORK_HOME/prefix/pfx"

# taskkill without /f: a WM_CLOSE to the game's window, which the game takes
# as its close button (suspend, drain the card, end).
echo "stop-game: asking the game to close (up to ${WAIT}s)..."
"$WINE" taskkill /im victory.exe >/dev/null 2>&1
for _ in $(seq "$WAIT"); do
    running || { echo "stop-game: the game closed."; exit 0; }
    sleep 1
done

# Still there (a hang, no window yet): an interrupt, which the game also
# takes as an orderly end.
echo "stop-game: no answer; sending an interrupt..."
pkill -INT -f 'victory\.exe'
for _ in $(seq 15); do
    running || { echo "stop-game: the game closed."; exit 0; }
    sleep 1
done

# Last resort. Wait for the card to go quiet first, so nothing of the game's
# is running on it when its memory goes.
echo "stop-game: still running; waiting for the GPU to go idle before killing it..."
for _ in $(seq 30); do
    util=$(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' ')
    [ -n "$util" ] && [ "$util" -lt 5 ] && break
    sleep 1
done
"$WINESERVER" -k 2>/dev/null
echo "stop-game: killed (GPU use was ${util:-unknown}%)."
