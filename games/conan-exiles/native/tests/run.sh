#!/usr/bin/env bash
# Tests: unit tests, then the real poller against a fake bridge. Default runs inside the build
# container; --native uses the host toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--native" ]; then
  docker build -q -t takaro-conan-native-build -f Dockerfile.build . >/dev/null
  exec docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" takaro-conan-native-build ./tests/run.sh --native
fi
CXX=${CXX:-g++}
FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Werror -Isrc -pthread)
mkdir -p tests/build
set -x
"$CXX" "${FLAGS[@]}" tests/unit_test.cpp src/common.cpp src/proto.cpp -o tests/build/unit_test
./tests/build/unit_test
"$CXX" "${FLAGS[@]}" -DTAKARO_CONAN_TEST tests/poller_test.cpp src/poller.cpp src/http.cpp src/common.cpp \
    src/proto.cpp src/gamethread.cpp -o tests/build/poller_test
python3 tests/fake_bridge_test.py tests/build/poller_test
