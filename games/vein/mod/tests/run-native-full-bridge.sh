#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--native" ]; then
  docker build -q -t takaro-vein-build -f Dockerfile.build . >/dev/null
  exec docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" \
    takaro-vein-build ./tests/run-native-full-bridge.sh --native
fi
prefix=${TAKARO_NATIVE_PREFIX:-/opt/takaro-native}
mkdir -p tests/build
${CXX:-g++} -std=c++17 -O1 -g -DTAKARO_BRIDGE_TEST -Isrc -I"$prefix/include" \
  tests/native_full_bridge.cpp src/native_bridge.cpp src/native_transport.cpp \
  src/native_behavior.cpp src/native_persistence.cpp src/native_log.cpp \
  src/common.cpp src/state.cpp src/events_parse.cpp src/actions_util.cpp \
  "$prefix/lib/libwebsockets.a" "$prefix/lib/libssl.a" "$prefix/lib/libcrypto.a" "$prefix/lib/libpcre2-8.a" \
  -pthread -ldl -o tests/build/native_full_bridge
python3 tests/native_full_bridge.py
