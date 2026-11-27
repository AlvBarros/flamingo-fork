#!/bin/sh
# Builds victory.exe (a Windows program) on Linux with cargo-xwin.
#
# No system LLVM is needed: Rust's own llvm-tools stand in for llvm-lib and
# rust-lld for lld-link, through links in $FORK_HOME/bin.
#
#   scripts/linux/build.sh            # release build
#   scripts/linux/build.sh --features diagnostics
#
# Needs: rustup, and `cargo install --locked cargo-xwin` (it downloads the
# Microsoft CRT and Windows SDK headers/libraries on first use, under
# Microsoft's license).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FORK_HOME="${FORK_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/flamingo-fork}"
TARGET=x86_64-pc-windows-msvc

cd "$ROOT"
command -v cargo-xwin >/dev/null || {
    echo "cargo-xwin is missing: cargo install --locked cargo-xwin" >&2
    exit 1
}
rustup target add "$TARGET" >/dev/null
rustup component add llvm-tools >/dev/null
TOOLS="$(rustc --print sysroot)/lib/rustlib/$(rustc -vV | sed -n 's/^host: //p')/bin"
mkdir -p "$FORK_HOME/bin"
ln -sf "$TOOLS/llvm-ar" "$FORK_HOME/bin/llvm-lib"
ln -sf "$TOOLS/llvm-ar" "$FORK_HOME/bin/llvm-ar"
ln -sf "$TOOLS/rust-lld" "$FORK_HOME/bin/lld-link"

# The scripted keys (`VICTORY_KEYS`, run.sh's FORK_KEYS) are built in: they do
# nothing unless set, and let a timing run press through the start screen
# alone. FORK_FEATURES="" builds without them.
# This base (foxhound via VIctory-NVIDIA) has the scripted keys in the
# diagnostics feature; that is also the build measured under Proton (frame
# median 58 ms, frames over 250 ms 5%).
FEATURES="${FORK_FEATURES-diagnostics}"
PATH="$FORK_HOME/bin:$PATH" cargo xwin build --release --locked -p victory --target "$TARGET" ${FEATURES:+--features "$FEATURES"} "$@"
echo "built: $ROOT/target/$TARGET/release/victory.exe"
