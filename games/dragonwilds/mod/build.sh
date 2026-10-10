#!/usr/bin/env bash
# Builds dist/libtakaro-dragonwilds.so inside the debian:bookworm toolchain container.
# Usage: ./build.sh [--native] [--tests]
#   --native  build on the host (needs g++ >= 10); default is docker
#   --tests   also build and run the unit tests
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

IMAGE=takaro-dragonwilds-build
if [ "$NATIVE" = 0 ]; then
  docker build -q -t "$IMAGE" -f Dockerfile.build . >/dev/null
  args=(--native)
  [ "$TESTS" = 1 ] && args+=(--tests)
  # DEBUG_CORRUPT_SIG must reach the compiler *inside* the container, otherwise the degrade build
  # is silently identical to the release one (El-Limon #75).
  env=()
  [ -n "${DEBUG_CORRUPT_SIG:-}" ] && env+=(-e "DEBUG_CORRUPT_SIG=$DEBUG_CORRUPT_SIG")
  exec docker run --rm -v "$PWD/..":/dragonwilds -w /dragonwilds/mod -u "$(id -u):$(id -g)" "${env[@]}" "$IMAGE" \
      ./build.sh "${args[@]}"
fi

CXX=${CXX:-g++}
CXXFLAGS=(-std=c++17 -O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra -Isrc)
LDFLAGS=(-shared -pthread -ldl -static-libstdc++ -static-libgcc "-Wl,--exclude-libs,ALL" \
         "-Wl,--version-script=exports.map")
NATIVE_PREFIX=${TAKARO_NATIVE_PREFIX:-/opt/takaro-native}
if [ ! -f "$NATIVE_PREFIX/lib/libwebsockets.a" ]; then
  echo "missing pinned static dependencies in $NATIVE_PREFIX; use ./build.sh in the Bookworm builder" >&2
  exit 1
fi
CXXFLAGS+=("-I$NATIVE_PREFIX/include")
LDFLAGS+=("$NATIVE_PREFIX/lib/libwebsockets.a" "$NATIVE_PREFIX/lib/libssl.a" \
          "$NATIVE_PREFIX/lib/libcrypto.a" "$NATIVE_PREFIX/lib/libpcre2-8.a")
# DEBUG_CORRUPT_SIG=<name> produces a deliberately broken build for the degrade proof.
if [ -n "${DEBUG_CORRUPT_SIG:-}" ]; then
  CXXFLAGS+=("-DTAKARO_DEBUG_CORRUPT_SIG=\"$DEBUG_CORRUPT_SIG\"")
fi

rm -rf build dist
mkdir -p build dist
for f in src/*.cpp; do
  echo "  CXX $f"
  "$CXX" "${CXXFLAGS[@]}" -c "$f" -o "build/$(basename "${f%.cpp}").o"
done
echo "  LD  dist/libtakaro-dragonwilds.so"
"$CXX" build/*.o "${LDFLAGS[@]}" -o dist/libtakaro-dragonwilds.so
strip --strip-unneeded dist/libtakaro-dragonwilds.so 2>/dev/null || true

# Refuse to produce a .so that LD_PRELOAD cannot load: the dynamic loader fails the *whole
# process* when a preloaded object has an unresolvable strong symbol (a VEIN artefact with an
# undefined `_ZN6Events4InitEv` crash-looped its server on 2026-09-17). Weak undefined symbols
# ('w': _ITM_*, __gmon_start__, __cxa_pure_virtual) are normal and resolve to 0.
undef="$(nm -D --undefined-only dist/libtakaro-dragonwilds.so | awk '$1 == "U" { print $2 }' \
          | grep -vE 'GLIBC|GCC|CXXABI' || true)"
if [ -n "$undef" ]; then
  echo "  !! undefined strong symbols in dist/libtakaro-dragonwilds.so:" >&2
  printf '%s\n' "$undef" | sed 's/^/     /' >&2
  echo "     LD_PRELOAD would crash-loop the server; refusing to ship this artefact." >&2
  rm -f dist/libtakaro-dragonwilds.so
  exit 1
fi
echo "  OK  no undefined strong symbols outside the C/C++ runtime"
( cd dist && sha256sum libtakaro-dragonwilds.so > SHA256SUMS )
echo "built dist/libtakaro-dragonwilds.so ($(stat -c %s dist/libtakaro-dragonwilds.so) bytes)"
cat dist/SHA256SUMS

if [ "$TESTS" = 1 ]; then
  ./tests/run.sh --native
  ./tests/run-timeout.sh --native
  ./tests/run-transport.sh --native
  ./tests/run-native-bridge.sh --native
  ./tests/run-native-behavior.sh --native
  ./tests/run-native-full-bridge.sh --native
fi
