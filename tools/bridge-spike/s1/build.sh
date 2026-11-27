#!/bin/sh
# Builds the S1 spike into $OUT (default: <repo>/target/bridge-spike-s1): the native test, and the
# Wine builtin (s1bridge.dll + s1bridge.so) with the Windows driver s1.exe beside it.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../../../target/bridge-spike-s1}
mkdir -p "$OUT"
gcc -O2 -Wall -o "$OUT/s1-native" "$HERE/s1_driver.c" "$HERE/s1_unix.c" -lpthread
gcc -O2 -Wall -shared -fPIC -o "$OUT/s1bridge.so" "$HERE/s1_unix.c"
x86_64-w64-mingw32-gcc -O2 -Wall -shared -o "$OUT/s1bridge.dll" "$HERE/s1_pe.c"
winebuild --builtin "$OUT/s1bridge.dll"
x86_64-w64-mingw32-gcc -O2 -Wall -o "$OUT/s1.exe" "$HERE/s1_driver.c"
echo "built in $OUT"
