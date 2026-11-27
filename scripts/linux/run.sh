#!/bin/sh
# Runs victory.exe under Proton's Wine (vkd3d-proton provides Direct3D 12).
#
#   scripts/linux/run.sh settings                    # what a run would use, nothing started
#   scripts/linux/run.sh check                       # GPU device check, no game needed
#   scripts/linux/run.sh prebuild [/path/to/shaders.rpf]
#   scripts/linux/run.sh play [/path/to/<the title's exe>] [victory run options...]
#
# Settings: scripts/linux/play.env (the tuned ones), then play.local.env
# (git-ignored: FORK_GAME and your own changes); a variable already set in the
# environment wins over both. Every log starts with the settings it ran with.
#   PROTON_DIR   Proton install (default: Proton Experimental, else 11.0)
#   FORK_HOME    prefix, cache and saves (default: ~/.local/share/flamingo-fork)
#   FORK_LOGS    run logs (default: logs/ in this repository, git-ignored)
#   FORK_GAME    the title's exe, for `play` with no path
#   FORK_PLAY_ARGS  victory run options when `play` is given none
#   VICTORY_*    passed through to victory.exe (see README.md)
#   FORK_D3DCOMPILER  Microsoft's d3dcompiler_47.dll, if not found (see env.sh)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# What the caller's environment already had, to tell its settings from ours.
FORK_CALLER_ENV="
$(env | grep -E '^(VICTORY|FLAMINGO|VKD3D|DXVK|MANGOHUD|__GL)[A-Z0-9_]*=' | cut -d= -f1)
"

# KEY=value lines; nothing in them is run. play.local.env is read first and a
# key already set is left alone, so: environment > play.local.env > play.env.
FORK_SETTINGS=""
fork_settings() {
    for f in "$ROOT/scripts/linux/play.local.env" "$ROOT/scripts/linux/play.env"; do
        [ -f "$f" ] || continue
        name=$(basename "$f")
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in '' | '#'*) continue ;; esac
            key=${line%%=*}
            val=${line#*=}
            case "$key" in
                "$line" | '' | [0-9]* | *[!A-Za-z0-9_]*)
                    echo "fork: $name: not KEY=value, skipped: $line" >&2; continue ;;
            esac
            case "$val" in '~/'*) val="$HOME/${val#'~/'}" ;; esac
            if eval "[ -n \"\${$key+set}\" ]"; then
                case "$FORK_SETTINGS" in
                    *"
$key="*) ;;                                    # a file before this one set it
                    *) eval "cur=\${$key}"
                       FORK_SETTINGS="$FORK_SETTINGS
$key=$cur	(environment)" ;;
                esac
                continue
            fi
            export "$key=$val"
            FORK_SETTINGS="$FORK_SETTINGS
$key=$val	($name)"
        done < "$f"
    done
}
fork_settings

. "$ROOT/scripts/linux/env.sh"
EXE="${FORK_EXE:-$ROOT/target/x86_64-pc-windows-msvc/release/victory.exe}"

[ -x "$PROTON_DIR/proton" ] || { echo "no Proton at $PROTON_DIR (set PROTON_DIR)" >&2; exit 1; }
[ -f "$EXE" ] || { echo "no $EXE: run scripts/linux/build.sh first" >&2; exit 1; }
LOGS="${FORK_LOGS:-$ROOT/logs}"
mkdir -p "$FORK_HOME/cache" "$FORK_HOME/data" "$FORK_HOME/prefix" "$LOGS"
[ -n "$__GL_SHADER_DISK_CACHE_PATH" ] && mkdir -p "$__GL_SHADER_DISK_CACHE_PATH"

# A Windows path for a Linux one (Wine maps / to Z:).
winpath() { printf 'Z:%s' "$(realpath "$1")" | tr / '\\'; }

# First run: let Proton create the prefix and install vkd3d-proton/DXVK into it.
if [ ! -d "$FORK_HOME/prefix/pfx" ]; then
    echo "creating the Wine prefix (once)..."
    if [ -n "$FORK_WINE" ]; then
        "$WINE" wineboot -u >/dev/null 2>&1 || true
    else
        STEAM_COMPAT_DATA_PATH="$FORK_HOME/prefix" \
        STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam" \
            "$PROTON_DIR/proton" run "$EXE" config >/dev/null 2>&1 || true
    fi
fi

# Proton's wine run directly does not apply the d3d12/dxgi overrides by
# itself; without them Wine's built-in d3d12 is used and device creation fails
# ("Invalid parameter"). Microsoft's d3dcompiler_47 too: see env.sh.
fork_compiler
fork_vkd3d
fork_overrides
export VICTORY_CACHE_DIR="$(winpath "$FORK_HOME/cache")"
export VICTORY_DATA_DIR="$(winpath "$FORK_HOME/data")"
# VICTORY_SKIP_PIPELINES=<hash>,...: pipelines with these shaders are never
# compiled (for a shader the driver crashes on); empty by default.
export VICTORY_SKIP_PIPELINES="${VICTORY_SKIP_PIPELINES-}"
LOG="$LOGS/$(date +%Y%m%d-%H%M%S)-${1:-run}.log"

# What this run is, at the top of its log (and on the terminal).
fork_header() {
    {
        echo "== run.sh $* ($(date '+%Y-%m-%d %H:%M:%S'))"
        echo "source     $(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo '?') (victory.exe built $(date -r "$EXE" '+%Y-%m-%d %H:%M'))"
        echo "proton     $PROTON_DIR ($(head -c 200 "$PROTON_DIR/version" 2>/dev/null | cut -d' ' -f2))"
        echo "wine       $WINE ($("$WINE" --version 2>/dev/null))"
        echo "vkd3d      $(cut -d' ' -f2- "$PROTON_DIR/files/lib/wine/vkd3d-proton/version" 2>/dev/null || echo '?')"
        echo "driver     NVIDIA $(grep -o '[0-9]\{3\}\.[0-9]*\.[0-9]*' /proc/driver/nvidia/version 2>/dev/null | head -1)"
        echo "fork_home  $FORK_HOME"
        [ -n "$FORK_RUN_WHAT" ] && echo "$FORK_RUN_WHAT"
        echo "settings   (source in brackets; the environment wins over play.local.env over play.env)"
        printf '%s\n' "$FORK_SETTINGS" | sed '/^$/d; s/^/  /'
        others=$(env | grep -E '^(VICTORY|FLAMINGO|VKD3D|DXVK|MANGOHUD|__GL)[A-Z0-9_]*=' | sort |
            while IFS= read -r kv; do
                case "$FORK_SETTINGS" in *"
${kv%%=*}="*) continue ;; esac
                case "$FORK_CALLER_ENV" in
                    *"
${kv%%=*}
"*) printf '  %s\t(environment)\n' "$kv" ;;
                    *) printf '  %s\t(run.sh)\n' "$kv" ;;
                esac
            done)
        [ -n "$others" ] && printf '%s\n' "$others"
        echo "=="
    } | tee "$LOG"
}

case "$1" in
settings)
    # What a run would use, without running anything (no log kept).
    FORK_RUN_WHAT="game       ${FORK_GAME:-(not set: put FORK_GAME in scripts/linux/play.local.env)}
run args   $FORK_PLAY_ARGS"
    LOG=/dev/null
    fork_header settings
    exit 0
    ;;
check)
    FORK_RUN_WHAT=""
    fork_header "$@"
    mkdir -p "$FORK_HOME/empty"
    "$WINE" "$EXE" translate --pipelines "$(winpath "$FORK_HOME/empty")" 2>&1 | tee -a "$LOG"
    ;;
prebuild)
    shaders="${2:-${FORK_SHADERS:-${FORK_GAME:+$(dirname "$FORK_GAME")/shaders.rpf}}}"
    [ -n "$shaders" ] || { echo "usage: $0 prebuild /path/to/shaders.rpf (or FORK_GAME in play.local.env)" >&2; exit 2; }
    [ -f "$shaders" ] || { echo "no $shaders" >&2; exit 2; }
    # Translates every shader into the run's cache (nothing is compiled on
    # the GPU). The translation depends on the host: the wave width and the
    # view heap's null slot a play log names ("host wave lanes", "host view
    # heaps hold N views": the null slot is N-1). play.env: an RTX 4070.
    export VICTORY_HOST_WAVE="${VICTORY_HOST_WAVE:-32}" VICTORY_VIEW_LIMIT="${VICTORY_VIEW_LIMIT:-999999}"
    FORK_RUN_WHAT="shaders    $shaders"
    fork_header "$@"
    "$WINE" "$EXE" translate --warm all --jobs "${JOBS:-8}" "$(winpath "$shaders")" 2>&1 | tee -a "$LOG"
    ;;
play)
    # A path first if given (anything not starting with -), else FORK_GAME.
    case "$2" in
        '' | -*) exe_path="$FORK_GAME"; shift 1 ;;
        *) exe_path="$2"; shift 2 ;;
    esac
    [ -n "$exe_path" ] || { echo "usage: $0 play /path/to/<the title's exe> [options] (or FORK_GAME in play.local.env)" >&2; exit 2; }
    [ -f "$exe_path" ] || { echo "no $exe_path" >&2; exit 2; }
    # No options given: FORK_PLAY_ARGS (word-split on purpose).
    # shellcheck disable=SC2086
    [ $# -gt 0 ] || set -- $FORK_PLAY_ARGS
    unset VICTORY_CAPTURE VICTORY_CAPTURE_FRAMES VICTORY_DUMP_TARGETS
    # FORK_KEYS (play.env): scripted presses, `<seconds>:<key>,...` after input
    # starts (Enter is the pad's A), for runs nobody watches; else none.
    if [ -n "$FORK_KEYS" ]; then export VICTORY_KEYS="$FORK_KEYS"; else unset VICTORY_KEYS; fi
    FORK_RUN_WHAT="game       $exe_path
run args   $*"
    fork_header play
    "$WINE" "$EXE" run "$@" "$(winpath "$exe_path")" --execute >> "$LOG" 2>&1 || true
    ;;
*)
    sed -n 4,7p "$0" >&2; exit 2
    ;;
esac
echo "log: $LOG"
