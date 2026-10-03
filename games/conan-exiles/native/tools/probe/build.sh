#!/usr/bin/env bash
# Builds build/libtakaro-conan-probe.so (dev tool, never shipped) in the stage 1 pinned Buster
# toolchain, with the same glibc <= 2.28 and no-undefined-symbol guards as ../../build.sh.
# Usage: tools/probe/build.sh [--native]
set -euo pipefail
cd "$(dirname "$0")"
NATIVE_DIR=$(cd ../.. && pwd)
if [ "${1:-}" != "--native" ]; then
  docker build -q -t takaro-conan-native-build -f "$NATIVE_DIR/Dockerfile.build" "$NATIVE_DIR" >/dev/null
  exec docker run --rm -v "$NATIVE_DIR":/src -w /src/tools/probe -u "$(id -u):$(id -g)" takaro-conan-native-build \
      ./build.sh --native
fi
export LC_ALL=C
CXX=${CXX:-g++}
CXXFLAGS=(-std=c++17 -O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra -Werror
          -I. -I../../src "-ffile-prefix-map=$PWD=." -fno-ident)
LDFLAGS=(-shared -pthread -static-libstdc++ -static-libgcc "-Wl,--exclude-libs,ALL"
         "-Wl,--version-script=../../exports.map" "-Wl,-z,relro" "-Wl,-z,now")
rm -rf build && mkdir -p build
for f in *.cpp ../../src/common.cpp ../../src/gamethread.cpp ../../src/proto.cpp; do
  echo "  CXX $f"
  "$CXX" "${CXXFLAGS[@]}" -c "$f" -o "build/$(basename "${f%.cpp}").o"
done
OUT=build/libtakaro-conan-probe.so
"$CXX" build/*.o "${LDFLAGS[@]}" -o "$OUT"
strip --strip-unneeded "$OUT"
undef="$(objdump -T "$OUT" | awk '/\*UND\*/ && $2 != "w"' | grep -vE 'GLIBC_|GCC_|CXXABI_' || true)"
[ -z "$undef" ] || { echo "undefined strong symbols: $undef" >&2; rm -f "$OUT"; exit 1; }
newest="$(objdump -T "$OUT" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
if [ "$(printf '%s\nGLIBC_2.28\n' "$newest" | sort -V | tail -1)" != "GLIBC_2.28" ]; then
  echo "needs $newest (> GLIBC_2.28)" >&2; rm -f "$OUT"; exit 1
fi
rm -f build/*.o
echo "built $OUT ($(stat -c %s "$OUT") bytes), newest glibc $newest"
