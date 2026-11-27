#!/bin/sh
# Builds the Unix half (../build.sh) plus a test-only PE half (victorybridge.dll, winebuild
# --builtin) and vb_check.exe into $1 (default: target/bridge), then runs vb_check.exe under
# Proton Experimental's Wine in its own throwaway prefix ($VB_PREFIX, default $OUT/pfx-ws1).
# Stops only that prefix's wineserver; never touches the game's prefix.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../../../target/bridge}
"$HERE/../build.sh" "$OUT"
x86_64-w64-mingw32-gcc -O2 -Wall -shared -o "$OUT/victorybridge.dll" "$HERE/vb_pe.c"
winebuild --builtin "$OUT/victorybridge.dll"
x86_64-w64-mingw32-gcc -O2 -Wall -o "$OUT/vb_check.exe" "$HERE/vb_check.c"
WINE=${WINE:-"$HOME/.steam/steam/steamapps/common/Proton - Experimental/files/bin/wine"}
export WINEPREFIX="${VB_PREFIX:-$OUT/pfx-ws1}" WINEDEBUG=-all
[ -d "$WINEPREFIX" ] || "$WINE" wineboot -i >/dev/null 2>&1
# Wine finds a builtin's Unix half only through WINEDLLPATH (S1, S4).
export WINEDLLPATH="$OUT"
cd "$OUT"
status=0
"$WINE" ./vb_check.exe || status=$?
"$(dirname "$WINE")/wineserver" -k || true
exit $status
