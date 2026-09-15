#!/usr/bin/env bash
# Cross-compile the Takaro Enshrouded plugin (dbghelp.dll proxy, x64 Windows) with zig 0.13.
set -euo pipefail
cd "$(dirname "$0")"
ZIG=${ZIG:-$HOME/.local/opt/zig-linux-x86_64-0.13.0/zig}
TARGET=x86_64-windows-gnu
OUT=build
DEFS=()
# DEBUG_CORRUPT_SIG=<signature name> builds a debug DLL into build-debug/ with that signature deliberately corrupted
if [ -n "${DEBUG_CORRUPT_SIG:-}" ]; then OUT=build-debug; DEFS=(-DTAKARO_DEBUG_CORRUPT_SIG="\"$DEBUG_CORRUPT_SIG\""); fi
mkdir -p "$OUT/obj"
MH=third_party/minhook
for f in $MH/src/hook.c $MH/src/buffer.c $MH/src/trampoline.c $MH/src/hde/hde64.c; do
  "$ZIG" cc -target $TARGET -O2 -I$MH/include -c "$f" -o "$OUT/obj/$(basename "$f" .c).o"
done
"$ZIG" c++ -target $TARGET -O2 -std=c++17 "${DEFS[@]}" -Wall -Wno-unused-function -I$MH/include -Isrc \
  -shared -s -o "$OUT/dbghelp.dll" src/*.cpp "$OUT"/obj/*.o -lws2_32
rm -f "$OUT"/*.lib "$OUT"/*.pdb
echo "built $OUT/dbghelp.dll ($(stat -c %s "$OUT/dbghelp.dll") bytes)"
