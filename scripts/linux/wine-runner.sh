#!/bin/sh
# Cargo runner: starts a Windows test (or program) under Proton's wine, with
# the same prefix and Direct3D 12 set-up as run.sh. Used by test.sh through
# CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUNNER (only on Linux; Windows runs
# tests natively).
. "$(dirname "$0")/env.sh"
[ -d "$WINEPREFIX" ] || {
    echo "no Wine prefix yet: run scripts/linux/run.sh check once" >&2
    exit 1
}
fork_compiler
fork_vkd3d
fork_overrides
# VICTORY_BRIDGE set and not 0: the bridge from FORK_BRIDGE_DIR (test.sh: the
# one build.sh put beside victory.exe) is copied beside the program, where
# victory-bridge's loader looks (Wine does not search WINEDLLPATH for a DLL
# loaded by name), and WINEDLLPATH names that directory. Unset or 0: nothing.
here=$(dirname "$1")
case "${VICTORY_BRIDGE:-0}" in
    0) ;;
    *) [ -n "$FORK_BRIDGE_DIR" ] && for f in victorybridge.dll victorybridge.so; do
           [ -f "$FORK_BRIDGE_DIR/$f" ] && ! cmp -s "$FORK_BRIDGE_DIR/$f" "$here/$f" &&
               cp -f "$FORK_BRIDGE_DIR/$f" "$here/$f"
       done ;;
esac
fork_bridge "$here"
case "$FORK_BRIDGE_STATE" in *missing*) echo "wine-runner: $FORK_BRIDGE_STATE" >&2 ;; esac
exec "$WINE" "$@"
