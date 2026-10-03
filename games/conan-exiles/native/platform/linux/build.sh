#!/usr/bin/env bash
# Builds dist/libtakaro-conan-native.so inside the pinned buster toolchain container.
# Usage: platform/linux/build.sh [--native] [--tests]   (run from anywhere)
#   --native  build with the toolchain of the current machine (the container calls this)
#   --tests   also build and run the host tests (tests/run.sh)
# Env: DEBUG_WRONG_BUILD_ID=1 builds a library pinned to a fake build-id (degrade proof only);
#      CONAN_SERVER_BINARY=<path> is mounted for the pins oracle test; catalog/conan-exiles is
#      mounted read-only for the pins.json <-> catalog check.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/../.."   # games/conan-exiles/native

NATIVE=0
TESTS=0
for a in "$@"; do
  case "$a" in
    --native) NATIVE=1 ;;
    --tests) TESTS=1 ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
  esac
done

IMAGE=takaro-conan-native-build:stage2
if [ "$NATIVE" = 0 ]; then
  docker build -q -t "$IMAGE" -f platform/linux/Dockerfile.build platform/linux >/dev/null
  args=(--native)
  [ "$TESTS" = 1 ] && args+=(--tests)
  env=()
  [ -n "${DEBUG_WRONG_BUILD_ID:-}" ] && env+=(-e "DEBUG_WRONG_BUILD_ID=$DEBUG_WRONG_BUILD_ID")
  mounts=()
  # tests/run.sh checks pins.json against the catalog targets, which live outside this tree.
  if [ -f ../../../catalog/conan-exiles/game.json ]; then
    mounts+=(-v "$(cd ../../../catalog/conan-exiles && pwd):/catalog:ro")
    env+=(-e CONAN_CATALOG_DIR=/catalog)
  fi
  if [ -n "${CONAN_SERVER_BINARY:-}" ]; then
    mounts+=(-v "$CONAN_SERVER_BINARY:/conan-server-binary:ro")
    env+=(-e CONAN_SERVER_BINARY=/conan-server-binary)
  fi
  exec docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" "${env[@]}" "${mounts[@]}" "$IMAGE" \
      platform/linux/build.sh "${args[@]}"
fi

CXX=${CXX:-g++}
PREFIX=${TAKARO_NATIVE_PREFIX:-/opt/takaro-native}
if [ ! -f "$PREFIX/lib/libwebsockets.a" ]; then
  echo "missing the pinned static dependencies in $PREFIX; run without --native (buster builder)" >&2
  exit 1
fi
# Globs below expand in C collation, so the object order (and the output bytes) never depend on
# the builder's locale.
export LC_ALL=C
# -ffile-prefix-map and a fixed build-id style keep two builds of one commit byte-identical.
CXXFLAGS=(-std=c++17 -O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra -Werror
          -Icore -Iplatform/linux "-I$PREFIX/include" "-ffile-prefix-map=$PWD=." -fno-ident)
LDFLAGS=(-shared -pthread -static-libstdc++ -static-libgcc "-Wl,--exclude-libs,ALL"
         "-Wl,--version-script=platform/linux/exports.map" "-Wl,--build-id=sha1" "-Wl,-z,relro" "-Wl,-z,now")
LIBS=("$PREFIX/lib/libwebsockets.a" "$PREFIX/lib/libssl.a" "$PREFIX/lib/libcrypto.a" -ldl)
if [ -n "${DEBUG_WRONG_BUILD_ID:-}" ]; then
  CXXFLAGS+=(-DTAKARO_DEBUG_WRONG_BUILD_ID)
fi

SOURCES=(core/*.cpp core/*/*.cpp platform/linux/*.cpp)
rm -rf build dist
mkdir -p build dist
objs=()
for f in "${SOURCES[@]}"; do
  o="build/$(echo "${f%.cpp}" | tr / _).o"
  echo "  CXX $f"
  "$CXX" "${CXXFLAGS[@]}" -c "$f" -o "$o"
  objs+=("$o")
done
echo "  LD  dist/libtakaro-conan-native.so"
"$CXX" "${objs[@]}" "${LDFLAGS[@]}" "${LIBS[@]}" -o dist/libtakaro-conan-native.so
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
  tests/run.sh --native
fi
