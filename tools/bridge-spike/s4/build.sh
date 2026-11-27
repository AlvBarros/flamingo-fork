#!/bin/sh
# Builds the S4 spike into $1 (default: target/bridge-spike-s4 under the repository) and,
# with RUN=1, runs it under Proton Experimental's Wine in its own prefix ($S4_PREFIX, default
# $OUT/pfx-s4), pinned to the P-cores (0-11 on this i5-14400F; override with S4_CPUS).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../../../target/bridge-spike-s4}
mkdir -p "$OUT"
gcc -O2 -Wall -shared -fPIC -o "$OUT/s4.so" "$HERE/s4_unix.c"
x86_64-w64-mingw32-gcc -O2 -Wall -shared -o "$OUT/s4.dll" "$HERE/s4_pe.c"
winebuild --builtin "$OUT/s4.dll"
x86_64-w64-mingw32-gcc -O2 -Wall -o "$OUT/s4.exe" "$HERE/s4.c"
[ "$RUN" = 1 ] || exit 0

WINE="$HOME/.steam/steam/steamapps/common/Proton - Experimental/files/bin/wine"
export WINEPREFIX=${S4_PREFIX:-$OUT/pfx-s4} WINEDEBUG=-all
[ -d "$WINEPREFIX" ] || taskset -c "${S4_CPUS:-0-11}" "$WINE" wineboot -i >/dev/null 2>&1
cd "$OUT"
# Without WINEDLLPATH naming the directory, Wine loads s4.dll as builtin but finds no Unix half
# (find_builtin_dll looks only in its own dll dirs and WINEDLLPATH for the .so).
export WINEDLLPATH="$OUT"
for mode in nop bridge wine; do
    echo "== $mode"
    taskset -c "${S4_CPUS:-0-11}" "$WINE" ./s4.exe $mode
done
"$(dirname "$WINE")/wineserver" -k || true
