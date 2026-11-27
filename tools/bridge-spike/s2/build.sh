#!/usr/bin/env bash
# Builds and runs spike S2 under Proton Experimental's Wine, in a throwaway prefix.
# Usage: build.sh [out-dir] [prefix]   (defaults: target/bridge-spike-s2, $out/pfx)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/../../../target/bridge-spike-s2}
mkdir -p "$out"
out=$(cd "$out" && pwd)
pfx=${2:-$out/pfx}
proton="$HOME/.steam/steam/steamapps/common/Proton - Experimental/files"
wine="$proton/bin/wine"

gcc -O2 -shared -fPIC -o "$out/s2bridge.so" "$here/s2_unix.c"
x86_64-w64-mingw32-gcc -O2 -shared -o "$out/s2bridge.dll" "$here/s2_pe.c"
winebuild --builtin "$out/s2bridge.dll"
x86_64-w64-mingw32-gcc -O1 -o "$out/s2.exe" "$here/s2.c"
x86_64-w64-mingw32-gcc -O1 -o "$out/s2d3d.exe" "$here/s2d3d.c" -lole32 -ldxguid

# Wine finds a builtin's Unix half (s2bridge.so) only through WINEDLLPATH.
export WINEPREFIX=$pfx WINEDEBUG=${WINEDEBUG:--all} WINEDLLPATH="$out"
if [ ! -d "$pfx/drive_c" ]; then "$wine" wineboot -i >/dev/null 2>&1; fi
# vkd3d-proton as scripts/linux/env.sh installs it; DXVK's dxgi (d3d12core imports dxgi).
cp -f "$proton/lib/wine/dxvk/x86_64-windows/dxgi.dll" \
   "$proton/lib/wine/vkd3d-proton/x86_64-windows/d3d12.dll" \
   "$proton/lib/wine/vkd3d-proton/x86_64-windows/d3d12core.dll" "$pfx/drive_c/windows/system32/"
cd "$out"
"$wine" ./s2.exe 2>&1 | tee s2.log || true
WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n,b" "$wine" ./s2d3d.exe 2>&1 | tee s2d3d.log || true
"$proton/bin/wineserver" -k || true
