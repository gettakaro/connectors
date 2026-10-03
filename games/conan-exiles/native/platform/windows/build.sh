#!/usr/bin/env bash
# Cross-compiles dist-windows/winmm.dll (x86_64 Windows, the winmm.dll proxy) with zig 0.13.0, the
# toolchain of the Enshrouded native connector. Reproducible: fixed source order, C collation,
# -ffile-prefix-map, zig writes no link timestamp, stripped. C exports only, so the
# DLL's C++ runtime never meets the MSVC-built server's.
# Usage: platform/windows/build.sh            (run from anywhere)
# Env:   ZIG=<path to zig 0.13.0>; DEBUG_WRONG_BUILD_ID=1 (degrade proof only)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/../.."   # games/conan-exiles/native
export LC_ALL=C

ZIG=${ZIG:-$HOME/.local/opt/zig-linux-x86_64-0.13.0/zig}
if ! command -v "$ZIG" >/dev/null 2>&1; then
  echo "zig 0.13.0 not found (set ZIG=...)" >&2
  exit 1
fi
ver="$("$ZIG" version)"
if [ "$ver" != "0.13.0" ]; then
  echo "need zig 0.13.0, found $ver" >&2
  exit 1
fi
( cd third_party/minhook && sha256sum --quiet -c ../minhook.sha256 ) || { echo "vendored MinHook does not match third_party/minhook.sha256" >&2; exit 1; }

TARGET=x86_64-windows-gnu
OUT=build-windows
DIST=dist-windows
rm -rf "$OUT" "$DIST"
mkdir -p "$OUT/obj" "$DIST"
MH=third_party/minhook
DEFS=()
[ -n "${DEBUG_WRONG_BUILD_ID:-}" ] && DEFS+=(-DTAKARO_DEBUG_WRONG_BUILD_ID)
PREFIX_MAP="-ffile-prefix-map=$PWD=."

for f in $MH/src/hook.c $MH/src/buffer.c $MH/src/trampoline.c $MH/src/hde/hde64.c; do
  echo "  CC  $f"
  "$ZIG" cc -target $TARGET -O2 "$PREFIX_MAP" -I$MH/include -c "$f" -o "$OUT/obj/mh_$(basename "$f" .c).o"
done
CXXFLAGS=(-target $TARGET -O2 -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter "$PREFIX_MAP"
          -Icore -Iplatform/windows -I$MH/include -DNOMINMAX "${DEFS[@]}")
SOURCES=(core/*.cpp core/*/*.cpp platform/windows/*.cpp)
objs=()
for f in "${SOURCES[@]}"; do
  o="$OUT/obj/$(echo "${f%.cpp}" | tr / _).o"
  echo "  CXX $f"
  "$ZIG" c++ "${CXXFLAGS[@]}" -c "$f" -o "$o"
  objs+=("$o")
done
echo "  LD  $DIST/winmm.dll"
"$ZIG" c++ -target $TARGET -shared -s -o "$DIST/winmm.dll" \
  "${objs[@]}" "$OUT"/obj/mh_*.o -lwinhttp -lcrypt32
rm -f "$DIST"/*.lib "$DIST"/*.pdb

# The proxy must export exactly the three winmm functions the server imports, and nothing else.
python3 - "$DIST/winmm.dll" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
pe, = struct.unpack_from("<I", d, 0x3C)
assert d[pe:pe + 4] == b"PE\0\0"
machine, nsec = struct.unpack_from("<HH", d, pe + 4)
opt = pe + 24
assert machine == 0x8664, "not x64"
exp_rva, exp_size = struct.unpack_from("<II", d, opt + 112)
secs = []
osz, = struct.unpack_from("<H", d, pe + 20)
for i in range(nsec):
    o = opt + osz + 40 * i
    vs, va, rs, ro = struct.unpack_from("<IIII", d, o + 8)
    secs.append((va, max(vs, rs), ro))
def off(rva):
    for va, sz, ro in secs:
        if va <= rva < va + sz:
            return rva - va + ro
    raise SystemExit("rva %x outside sections" % rva)
e = off(exp_rva)
n_names, = struct.unpack_from("<I", d, e + 24)
names_rva, = struct.unpack_from("<I", d, e + 32)
names = []
for i in range(n_names):
    r, = struct.unpack_from("<I", d, off(names_rva) + 4 * i)
    s = off(r)
    names.append(d[s:d.index(b"\0", s)].decode())
want = ["timeBeginPeriod", "timeEndPeriod", "timeGetTime"]
# zig's bundled libunwind marks its C API dllexport on Windows; those exports are inert. Anything else
# (or a missing winmm function) fails the build.
extra = [n for n in names if n not in want and not n.startswith(("_Unwind_", "unw_", "_GCC_specific_handler"))]
if extra or any(w not in names for w in want):
    raise SystemExit("  !! exports are %s, expected %s (+ libunwind)" % (names, want))
print("  OK  exports: " + ", ".join(want) + " (+ %d libunwind)" % (len(names) - len(want)))
# Reproducible bytes: zero the link timestamps (COFF header, export and debug directories).
b = bytearray(d)
struct.pack_into("<I", b, pe + 8, 0)
struct.pack_into("<I", b, e + 4, 0)
dbg_rva, dbg_size = struct.unpack_from("<II", d, opt + 112 + 6 * 8)
if dbg_rva:
    for k in range(dbg_size // 28):
        struct.pack_into("<I", b, off(dbg_rva) + 28 * k + 4, 0)
struct.pack_into("<I", b, opt + 64, 0)  # CheckSum (not verified for user-mode DLLs)
open(sys.argv[1], "wb").write(bytes(b))
PY
( cd "$DIST" && sha256sum winmm.dll > SHA256SUMS )
echo "built $DIST/winmm.dll ($(stat -c %s "$DIST/winmm.dll") bytes)"
cat "$DIST/SHA256SUMS"
