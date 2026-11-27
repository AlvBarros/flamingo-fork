#!/bin/sh
# Builds the S3 spike into $1 (default: target/bridge-spike-s3 under the repo) and, with RUN=1, runs
# it: natively, then under Proton Experimental's Wine in a throwaway prefix ($1/pfx-s3) with
# Proton's vkd3d-proton as d3d12. Needs gcc, x86_64-w64-mingw32-gcc, winebuild, and Vulkan headers
# (VK_INC, default /usr/include; any directory holding vulkan/vulkan.h).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/../../../target/bridge-spike-s3}
mkdir -p "$out"
out=$(cd "$out" && pwd)
inc=${VK_INC:-/usr/include}
proton=${PROTON:-$HOME/.steam/steam/steamapps/common/Proton - Experimental}

gcc -O2 -Wall -I"$inc" -o "$out/vkhost" "$here/vkhost.c" -ldl
gcc -O2 -Wall -shared -fPIC -o "$out/s3bridge.so" "$here/s3bridge_unix.c"
x86_64-w64-mingw32-gcc -O2 -Wall -shared -o "$out/s3bridge.dll" "$here/s3bridge_pe.c"
winebuild --builtin "$out/s3bridge.dll"
x86_64-w64-mingw32-gcc -O2 -Wall -I"$inc" -o "$out/vkhost.exe" "$here/vkhost.c"
x86_64-w64-mingw32-gcc -O2 -Wall -o "$out/d3d12host.exe" "$here/d3d12host.c" -ld3d12 -ldxgi -lole32
echo "built into $out"

[ "${RUN:-0}" = 1 ] || exit 0
echo "== native"
"$out/vkhost" || true
export WINEPREFIX="$out/pfx-s3" WINEDEBUG=${WINEDEBUG:--all}
wine="$proton/files/bin/wine"
if [ ! -d "$WINEPREFIX/drive_c" ]; then "$wine" wineboot -i; fi
# vkd3d-proton's d3d12/d3d12core and DXVK's dxgi (d3d12core needs a dxgi; Wine's own pulls in
# wined3d and its libvkd3d, which Proton does not ship). Replaced, never written through: a prefix
# file may be a link into the Proton install.
sys32="$WINEPREFIX/drive_c/windows/system32"
for f in vkd3d-proton/x86_64-windows/d3d12.dll vkd3d-proton/x86_64-windows/d3d12core.dll \
         dxvk/x86_64-windows/dxgi.dll dxvk/x86_64-windows/d3d11.dll; do
    rm -f "$sys32/$(basename "$f")" && install -m 755 "$proton/files/lib/wine/$f" "$sys32/"
done
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi,d3d11=n,b"
# The Unix half (s3bridge.so) is found only through WINEDLLPATH; beside the .dll is not enough.
export WINEDLLPATH="$out"
cd "$out"
echo "== Proton: Vulkan (winevulkan)"
"$wine" ./vkhost.exe || true
echo "== Proton: D3D12 (vkd3d-proton)"
VKD3D_DEBUG=${VKD3D_DEBUG:-warn} "$wine" ./d3d12host.exe || true
"$proton/files/bin/wineserver" -k || true
