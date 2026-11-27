#!/bin/sh
# The PE half's loading test (WS2), under Proton's Wine in a throwaway prefix
# of its own (never the game's): builds victorybridge.dll with the stub Unix
# half (stub_unix.c) and load.exe beside them, then checks
#   1. beside, WINEDLLPATH = that directory: the bridge answers VB_VERSION;
#   2. beside, no WINEDLLPATH: the DLL loads but every call is refused
#      (Wine finds no Unix half), so victory-bridge would report it off;
#   3. by bare name from a program elsewhere (and run from there),
#      WINEDLLPATH set: not found
#      (Wine does not look on WINEDLLPATH for a DLL loaded by name: the
#      files must be beside the program, which is what build.sh and
#      wine-runner.sh do);
# then, unless BRIDGE_PROBE=0 (needs cargo-xwin, as build.sh), the Rust side
# itself: crates/victory-bridge's example `bridge_probe` beside the DLL says
# victory_bridge::available() is true with VICTORY_BRIDGE=1 and WINEDLLPATH,
# and false with VICTORY_BRIDGE unset, 0, without WINEDLLPATH, or with the
# program elsewhere.
#
#   bridge/pe/test/run.sh [OUTDIR]       # default: target/bridge-pe-test
#
# PROTON_DIR: Proton install (default: Proton - Experimental, else 11.0).
# At the end it stops only its own prefix's wineserver.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${1:-$ROOT/target/bridge-pe-test}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

"$HERE/../build.sh" "$OUT" "$HERE/stub_unix.c"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -std=gnu11 -I"$ROOT/bridge/unix" \
    -o "$OUT/load.exe" "$HERE/load.c"

if [ -z "$PROTON_DIR" ]; then
    for p in "Proton - Experimental" "Proton 11.0"; do
        PROTON_DIR="$HOME/.steam/steam/steamapps/common/$p"
        [ -x "$PROTON_DIR/files/bin/wine" ] && break
    done
fi
WINE="$PROTON_DIR/files/bin/wine"
[ -x "$WINE" ] || { echo "no Proton Wine at $WINE (set PROTON_DIR)" >&2; exit 1; }
export WINEPREFIX="$OUT/pfx" WINEDEBUG=-all
unset WINEDLLPATH WINEDLLOVERRIDES
[ -d "$WINEPREFIX" ] || "$WINE" wineboot -i >/dev/null 2>&1
trap '"$(dirname "$WINE")/wineserver" -k 2>/dev/null || true' EXIT

echo "wine: $("$WINE" --version)"
cd "$OUT"
fail=0
check() {
    echo "== $1"
    shift
    if "$@"; then :; else fail=1; fi
}
check "beside victory.exe's place, WINEDLLPATH set" env WINEDLLPATH="$OUT" "$WINE" ./load.exe beside expect-on
check "beside, no WINEDLLPATH (no Unix half found)" "$WINE" ./load.exe beside expect-off
mkdir -p "$OUT/elsewhere" && cp "$OUT/load.exe" "$OUT/elsewhere/"
check "by name from elsewhere, WINEDLLPATH set" env -C "$OUT/elsewhere" WINEDLLPATH="$OUT" "$WINE" ./load.exe name expect-missing
if [ "${BRIDGE_PROBE:-1}" != 0 ]; then
    FORK_HOME="${FORK_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/flamingo-fork}"
    (cd "$ROOT" && PATH="$FORK_HOME/bin:$PATH" cargo xwin build --release --locked --quiet \
        -p victory-bridge --example bridge_probe --target x86_64-pc-windows-msvc)
    cp "${CARGO_TARGET_DIR:-$ROOT/target}/x86_64-pc-windows-msvc/release/examples/bridge_probe.exe" "$OUT/"
    check "Rust: VICTORY_BRIDGE=1, WINEDLLPATH set" env VICTORY_BRIDGE=1 WINEDLLPATH="$OUT" "$WINE" ./bridge_probe.exe on
    check "Rust: VICTORY_BRIDGE unset, WINEDLLPATH set" env -u VICTORY_BRIDGE WINEDLLPATH="$OUT" "$WINE" ./bridge_probe.exe off
    check "Rust: VICTORY_BRIDGE=0, WINEDLLPATH set" env VICTORY_BRIDGE=0 WINEDLLPATH="$OUT" "$WINE" ./bridge_probe.exe off
    check "Rust: VICTORY_BRIDGE=1, no WINEDLLPATH" env VICTORY_BRIDGE=1 "$WINE" ./bridge_probe.exe off
    cp "$OUT/bridge_probe.exe" "$OUT/elsewhere/"
    check "Rust: VICTORY_BRIDGE=1, WINEDLLPATH set, program elsewhere" env -C "$OUT/elsewhere" VICTORY_BRIDGE=1 WINEDLLPATH="$OUT" "$WINE" ./bridge_probe.exe off
fi
[ $fail = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
