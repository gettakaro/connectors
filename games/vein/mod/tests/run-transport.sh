#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--native" ]; then
  docker build -q -t takaro-vein-build -f Dockerfile.build . >/dev/null
  exec docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" \
    takaro-vein-build ./tests/run-transport.sh --native
fi
prefix=${TAKARO_NATIVE_PREFIX:-/opt/takaro-native}
mkdir -p tests/build
${CXX:-g++} -std=c++17 -O1 -g -Isrc -I"$prefix/include" \
  tests/native_transport_tls.cpp src/native_transport.cpp \
  "$prefix/lib/libwebsockets.a" "$prefix/lib/libssl.a" "$prefix/lib/libcrypto.a" \
  -pthread -ldl -o tests/build/native_transport_tls
python3 tests/native_transport_tls.py
${CXX:-g++} -std=c++17 -O1 -g -Isrc -I"$prefix/include" \
  tests/native_game_http.cpp src/native_transport.cpp \
  "$prefix/lib/libwebsockets.a" "$prefix/lib/libssl.a" "$prefix/lib/libcrypto.a" \
  -pthread -ldl -o tests/build/native_game_http
python3 tests/native_game_http.py
