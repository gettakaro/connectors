#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--native" ]; then
  docker build -q -t takaro-vein-build -f Dockerfile.build . >/dev/null
  exec docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" \
    takaro-vein-build ./tests/run-native-behavior.sh --native
fi
prefix=${TAKARO_NATIVE_PREFIX:-/opt/takaro-native}
mkdir -p tests/build
${CXX:-g++} -std=c++17 -O1 -g -DTAKARO_BRIDGE_TEST -Isrc -I"$prefix/include" \
  tests/native_behavior_parity.cpp src/native_behavior.cpp src/native_persistence.cpp src/native_log.cpp src/events_parse.cpp src/actions_util.cpp \
  "$prefix/lib/libpcre2-8.a" -pthread -o tests/build/native_behavior_parity
tests/build/native_behavior_parity
