#!/usr/bin/env bash
# Unit tests. Default runs inside the build container; --native uses the host toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--native" ]; then
  docker build -q -t takaro-vein-build -f Dockerfile.build . >/dev/null
  exec docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" takaro-vein-build ./tests/run.sh --native
fi
CXX=${CXX:-g++}
mkdir -p tests/build
set -x
$CXX -std=c++17 -O1 -g -Wall -Wextra -Isrc -Itests \
    tests/unit_test.cpp src/common.cpp src/state.cpp src/resolve.cpp src/events_parse.cpp src/actions_util.cpp src/config_file.cpp -o tests/build/unit_test -pthread -ldl
./tests/build/unit_test
