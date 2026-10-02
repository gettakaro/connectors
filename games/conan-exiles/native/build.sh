#!/usr/bin/env bash
# Builds dist/libtakaro-conan-native.so inside the pinned Buster toolchain container.
# Usage: ./build.sh [--native] [--tests]
#   --native  build on the host (needs g++ >= 10); default is docker
#   --tests   also build and run the tests
set -euo pipefail
cd "$(dirname "$0")"

NATIVE=0
TESTS=0
for a in "$@"; do
  case "$a" in
    --native) NATIVE=1 ;;
    --tests) TESTS=1 ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
  esac
done

IMAGE=takaro-conan-native-build
if [ "$NATIVE" = 0 ]; then
  docker build -q -t "$IMAGE" -f Dockerfile.build . >/dev/null
  args=(--native)
  [ "$TESTS" = 1 ] && args+=(--tests)
  env=()
  [ -n "${DEBUG_WRONG_BUILD_ID:-}" ] && env+=(-e "DEBUG_WRONG_BUILD_ID=$DEBUG_WRONG_BUILD_ID")
  exec docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" "${env[@]}" "$IMAGE" \
      ./build.sh "${args[@]}"
fi

CXX=${CXX:-g++}
# -ffile-prefix-map and a fixed build-id style keep two builds of one commit byte-identical.
CXXFLAGS=(-std=c++17 -O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra -Werror -Isrc
          "-ffile-prefix-map=$PWD=." -fno-ident)
LDFLAGS=(-shared -pthread -static-libstdc++ -static-libgcc "-Wl,--exclude-libs,ALL"
         "-Wl,--version-script=exports.map" -Wl,--build-id=sha1 -Wl,-z,relro -Wl,-z,now)
# DEBUG_WRONG_BUILD_ID=1 builds a library that expects another server build, for the proof that
# a mismatched server runs unhooked.
if [ -n "${DEBUG_WRONG_BUILD_ID:-}" ]; then
  CXXFLAGS+=(-DTAKARO_DEBUG_WRONG_BUILD_ID)
fi

rm -rf build dist
mkdir -p build dist
for f in $(ls src/*.cpp | LC_ALL=C sort); do
  echo "  CXX $f"
  "$CXX" "${CXXFLAGS[@]}" -c "$f" -o "build/$(basename "${f%.cpp}").o"
done
echo "  LD  dist/libtakaro-conan-native.so"
"$CXX" $(ls build/*.o | LC_ALL=C sort) "${LDFLAGS[@]}" -o dist/libtakaro-conan-native.so
strip --strip-unneeded dist/libtakaro-conan-native.so

# The dynamic loader fails the *whole server process* when a preloaded object has an
# unresolvable strong symbol (VEIN shipped one on 2026-09-17), so refuse to produce one.
# objdump -T rather than nm -D: Buster's nm does not print symbol versions. Weak undefined
# symbols (_ITM_*, __gmon_start__) are normal and resolve to 0.
undef="$(objdump -T dist/libtakaro-conan-native.so | awk '/\*UND\*/ && $2 != "w"' \
          | grep -vE 'GLIBC_|GCC_|CXXABI_' || true)"
if [ -n "$undef" ]; then
  echo "  !! undefined strong symbols in dist/libtakaro-conan-native.so:" >&2
  printf '%s\n' "$undef" | sed 's/^/     /' >&2
  rm -f dist/libtakaro-conan-native.so
  exit 1
fi
# The server binary needs glibc 2.28; the library must not need anything newer.
newest="$(objdump -T dist/libtakaro-conan-native.so | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
if [ "$(printf '%s\nGLIBC_2.28\n' "$newest" | sort -V | tail -1)" != "GLIBC_2.28" ]; then
  echo "  !! the library needs ${newest}; the Conan server only needs GLIBC_2.28" >&2
  rm -f dist/libtakaro-conan-native.so
  exit 1
fi
echo "  OK  no undefined strong symbols; newest glibc symbol version: ${newest}"
exported="$(nm -D --defined-only dist/libtakaro-conan-native.so | awk '$2 ~ /[TDB]/ { print $3 }')"
if [ -n "$exported" ]; then
  echo "  !! the library exports symbols: $exported" >&2
  exit 1
fi
( cd dist && sha256sum libtakaro-conan-native.so > SHA256SUMS )
echo "built dist/libtakaro-conan-native.so ($(stat -c %s dist/libtakaro-conan-native.so) bytes)"
cat dist/SHA256SUMS

if [ "$TESTS" = 1 ]; then
  ./tests/run.sh --native
fi
