#!/bin/sh
# Builds the bridge's Unix half and its native test into $1 (default: target/bridge under the
# repository): victorybridge.so (the Unix half Wine loads beside victorybridge.dll) and
# test_unix (loads the .so with dlopen, no Wine). With RUN=1 also runs the test; with SPEED=1
# the 60,000-view timings too.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../../target/bridge}
mkdir -p "$OUT"
CFLAGS="-O2 -g -Wall -Wextra -Werror -std=gnu11"
gcc $CFLAGS -shared -fPIC -fvisibility=hidden -Wl,-z,defs -o "$OUT/victorybridge.so" \
    "$HERE/victorybridge_unix.c" -lpthread
gcc $CFLAGS -o "$OUT/test_unix" "$HERE/test_unix.c" -ldl -lpthread
echo "built $OUT/victorybridge.so $OUT/test_unix"
if [ "${RUN:-0}" = 1 ]; then
    if [ "${SPEED:-0}" = 1 ]; then
        "$OUT/test_unix" "$OUT/victorybridge.so" --speed
    else
        "$OUT/test_unix" "$OUT/victorybridge.so"
    fi
fi
