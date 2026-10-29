#!/usr/bin/env bash
# Research probes for #298 (C4, native game-thread runners). Each probe compiles one connector's
# production gamethread.cpp with AddressSanitizer inside the Dune build image (debian:bookworm,
# g++ 12) and drives a started-job-after-timeout scenario. Off by default: nothing in CI or the
# release builds references this folder. Usage: run.sh [dune|conan|all] (default all).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../../../.." && pwd)"
which="${1:-all}"
img=takaro-dune-build
docker build -q -t "$img" -f "$repo/games/dune/mod/Dockerfile.build" "$repo/games/dune/mod" >/dev/null
probe=docs/research/architecture/probes/native-runner
run() { docker run --rm -v "$repo":/repo -w /repo -u "$(id -u):$(id -g)" "$img" bash -c "$1"; }
flags="-std=c++17 -O0 -g -fsanitize=address -fno-omit-frame-pointer -pthread -ldl"
rc=0
if [ "$which" = all ] || [ "$which" = dune ]; then
  echo "### dune: RunJson started job outlives the caller"
  run "mkdir -p /tmp/b && g++ $flags -Igames/dune/mod/src $probe/dune_runjson_lifetime.cpp $probe/dune_stubs.cpp \
    games/dune/mod/src/gamethread.cpp games/dune/mod/src/common.cpp games/dune/mod/src/state.cpp \
    games/dune/mod/src/perf.cpp -o /tmp/b/p && ASAN_OPTIONS=detect_stack_use_after_return=1 /tmp/b/p" || rc=$?
  echo "dune probe exit=$rc"
fi
if [ "$which" = all ] || [ "$which" = conan ]; then
  echo "### conan: Run waits for a started job past its timeout"
  c=0
  run "mkdir -p /tmp/b && g++ $flags -Igames/conan-exiles/native/core $probe/conan_running_timeout.cpp \
    games/conan-exiles/native/core/gamethread.cpp games/conan-exiles/native/core/common.cpp \
    games/conan-exiles/native/core/gtstats.cpp games/conan-exiles/native/core/takaro/json_util.cpp -o /tmp/b/p && /tmp/b/p" || c=$?
  echo "conan probe exit=$c"
  [ $rc -eq 0 ] && rc=$c
fi
exit $rc
