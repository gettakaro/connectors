#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--native" ]; then
  docker build -q -t takaro-vein-build -f Dockerfile.build . >/dev/null
  exec docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" takaro-vein-build ./tests/run-timeout.sh --native
fi
mkdir -p tests/build
${CXX:-g++} -std=c++17 -O1 -g -fsanitize=address -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -DTAKARO_GAMETHREAD_TEST -Isrc \
  tests/game_thread_timeout_test.cpp src/gamethread.cpp src/common.cpp src/state.cpp src/perf.cpp \
  -Wl,--gc-sections -pthread -ldl -o tests/build/game_thread_timeout_test
ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1 ./tests/build/game_thread_timeout_test
