# Shared Wine/Proton set-up for run.sh and wine-runner.sh (sourced, not run).
#
# Sets FORK_HOME, PROTON_DIR, WINEPREFIX, WINEDEBUG, WINEDLLOVERRIDES and WINE,
# and makes sure the prefix uses Microsoft's d3dcompiler_47.dll.
#
# Why d3dcompiler_47: the layer compiles a few compute shaders at run time
# with D3DCompile (the indirect-argument clamp, its worker, the card compare,
# the FFX upscale). Wine's built-in compiler rejects them (no Load4 or
# Interlocked* on RWByteAddressBuffer) and ignores their RootSignature
# attribute; without the clamp every ExecuteIndirect is dropped. Microsoft's
# DLL cannot be shipped here: give it as FORK_D3DCOMPILER=/path/to/it, put it
# at $FORK_HOME/d3dcompiler_47.dll, or let this look in your Steam games.

FORK_HOME="${FORK_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/flamingo-fork}"
# Proton Experimental first: Proton 11.0's vkd3d-proton (g212991fc2) takes the
# process down compiling some of the title's compute pipelines on NVIDIA (no
# exception, the process is gone: 4e296b1885c37b29 at the first load, more in
# the first world frame); Experimental's (g2576c0b4a) compiles them all.
if [ -z "$PROTON_DIR" ]; then
    for p in "Proton - Experimental" "Proton 11.0"; do
        PROTON_DIR="$HOME/.steam/steam/steamapps/common/$p"
        [ -x "$PROTON_DIR/proton" ] && break
    done
fi
export WINEPREFIX="$FORK_HOME/prefix/pfx"
export WINEDEBUG="${WINEDEBUG:--all}"
# vkd3d-proton's own pipeline cache, kept out of the working directory.
export VKD3D_SHADER_CACHE_PATH="${VKD3D_SHADER_CACHE_PATH:-$FORK_HOME/vkd3d-cache}"
mkdir -p "$VKD3D_SHADER_CACHE_PATH"
# FORK_WINE: another Wine than Proton's (e.g. /usr/bin/wine), for a Wine
# whose development files are installed (winegcc); Proton still provides
# vkd3d-proton and DXVK, copied into the prefix (fork_vkd3d).
WINE="${FORK_WINE:-$PROTON_DIR/files/bin/wine}"

# True for a Microsoft build of the DLL (Wine's own says "Wine builtin DLL").
fork_is_ms_compiler() {
    [ -f "$1" ] && ! grep -q 'Wine builtin DLL' "$1" && grep -q 'D3DCompile' "$1"
}

fork_find_compiler() {
    [ -n "$FORK_D3DCOMPILER" ] && { echo "$FORK_D3DCOMPILER"; return; }
    [ -f "$FORK_HOME/d3dcompiler_47.dll" ] && { echo "$FORK_HOME/d3dcompiler_47.dll"; return; }
    for lib in "$HOME/.local/share/Steam/steamapps/common" "$HOME/.steam/steam/steamapps/common"; do
        [ -d "$lib" ] && find "$lib" -iname d3dcompiler_47.dll -ipath '*win64*' 2>/dev/null
    done | grep -viE '/proton|/files/lib' | while read -r f; do
        fork_is_ms_compiler "$f" && { echo "$f"; break; }
    done
}

fork_compiler() {
    sys32="$WINEPREFIX/drive_c/windows/system32/d3dcompiler_47.dll"
    if ! fork_is_ms_compiler "$sys32"; then
        src=$(fork_find_compiler)
        if [ -n "$src" ] && fork_is_ms_compiler "$src"; then
            cp "$src" "$FORK_HOME/d3dcompiler_47.dll" 2>/dev/null || true
            cp "$src" "$sys32"
            echo "fork: Microsoft d3dcompiler_47.dll installed in the prefix (from $src)" >&2
        else
            echo "fork: WARNING: no Microsoft d3dcompiler_47.dll found; with Wine's own," >&2
            echo "      indirect draws are dropped. Set FORK_D3DCOMPILER=/path/to/d3dcompiler_47.dll" >&2
            return
        fi
    fi
    FORK_EXTRA_OVERRIDES="d3dcompiler_47=n"
}

# The prefix keeps the vkd3d-proton it was made with (run.sh starts Wine
# directly, not through `proton`, which would update it): copy PROTON_DIR's
# d3d12.dll and d3d12core.dll in when they differ, so changing PROTON_DIR
# changes the vkd3d-proton that runs.
fork_vkd3d() {
    src="$PROTON_DIR/files/lib/wine/vkd3d-proton/x86_64-windows"
    dst="$WINEPREFIX/drive_c/windows/system32"
    [ -d "$src" ] && [ -d "$dst" ] || return 0
    # With FORK_WINE the prefix has the Wine's own dxgi/d3d11 (wined3d):
    # DXVK's from Proton go in too, as `proton` puts them in its prefixes.
    dxvk="$PROTON_DIR/files/lib/wine/dxvk/x86_64-windows"
    if [ -n "$FORK_WINE" ] && [ -d "$dxvk" ]; then
        for dll in dxgi.dll d3d11.dll; do
            cmp -s "$dxvk/$dll" "$dst/$dll" || { rm -f "$dst/$dll" && install -m 755 "$dxvk/$dll" "$dst/$dll"; }
        done
    fi
    for dll in d3d12.dll d3d12core.dll; do
        if [ -f "$src/$dll" ] && ! cmp -s "$src/$dll" "$dst/$dll"; then
            # Replaced, not written through: Proton ships them read-only, and
            # a symlink into the Proton install must not be written into.
            rm -f "$dst/$dll" && install -m 755 "$src/$dll" "$dst/$dll" &&
                echo "fork: $dll from $(cut -d' ' -f2- "$src/../version" 2>/dev/null || echo "$src")" >&2
        fi
    done
}

fork_overrides() {
    o="d3d12,d3d12core,dxgi,d3d11=n,b"
    [ -n "$FORK_EXTRA_OVERRIDES" ] && o="$o;$FORK_EXTRA_OVERRIDES"
    export WINEDLLOVERRIDES="$o${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
}
