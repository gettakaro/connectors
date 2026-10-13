#!/usr/bin/env bash
# Unit tests. Default runs inside the build container; --native uses the host toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--native" ]; then
  docker build -q -t takaro-dragonwilds-build -f Dockerfile.build . >/dev/null
  exec docker run --rm -v "$PWD/..":/dragonwilds -w /dragonwilds/mod -u "$(id -u):$(id -g)" takaro-dragonwilds-build ./tests/run.sh --native
fi
CXX=${CXX:-g++}
mkdir -p tests/build
set -x
$CXX -std=c++17 -O1 -g -Wall -Wextra -Isrc -Itests \
    tests/unit_test.cpp src/common.cpp src/config_file.cpp src/state.cpp src/perf.cpp -o tests/build/unit_test -pthread
./tests/build/unit_test
