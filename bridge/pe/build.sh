#!/bin/sh
# Builds the memory bridge's DLL into OUTDIR: victorybridge.dll (this PE half,
# MinGW + `winebuild --builtin`) and victorybridge.so (the Unix half, gcc).
#
#   bridge/pe/build.sh OUTDIR [unix-half.c ...]
#
# The Unix half's sources default to bridge/unix/*.c (any with "test" in the
# name left out); with none there, nothing is built and this says so (exit 0).
# Both files are made in a scratch directory and moved into OUTDIR only when
# both built, so a failure leaves OUTDIR as it was. Exit 1 on a failure.
# BRIDGE_CFLAGS / BRIDGE_UNIX_LIBS: extra gcc flags / libraries for the Unix half.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
UNIX_DIR="$HERE/../unix"
OUT=${1:?usage: bridge/pe/build.sh OUTDIR [unix-half.c ...]}
shift

if [ $# -eq 0 ]; then
    for f in "$UNIX_DIR"/*.c; do
        [ -f "$f" ] || continue
        case "$(basename "$f")" in *test*) continue ;; esac
        set -- "$@" "$f"
    done
fi
if [ $# -eq 0 ]; then
    echo "bridge: no Unix half in bridge/unix/ yet: victorybridge.dll/.so not built" >&2
    exit 0
fi
for tool in x86_64-w64-mingw32-gcc winebuild gcc; do
    command -v "$tool" >/dev/null || { echo "bridge: $tool is missing" >&2; exit 1; }
done

mkdir -p "$OUT"
TMP=$(mktemp -d "$OUT/.victorybridge.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -std=gnu11 -shared -static-libgcc \
    -o "$TMP/victorybridge.dll" "$HERE/victorybridge_pe.c"
winebuild --builtin "$TMP/victorybridge.dll"
# shellcheck disable=SC2086
gcc -O2 -Wall -Wextra -std=gnu11 -shared -fPIC -I"$UNIX_DIR" $BRIDGE_CFLAGS \
    -o "$TMP/victorybridge.so" "$@" $BRIDGE_UNIX_LIBS
mv -f "$TMP/victorybridge.dll" "$TMP/victorybridge.so" "$OUT/"
echo "bridge: $OUT/victorybridge.dll + victorybridge.so"
