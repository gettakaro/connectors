#!/usr/bin/env bash
# Host-side tests with a tiny windows.h shim. Needs docker.
#   state_test: log-line correlator (state.cpp)
#   moderation_test: canKickBan admin-protection bypass helpers; set ENSHROUDED_EXE=<path to enshrouded_server.exe>
#                    to also check the anchors against the pinned binary.
set -euo pipefail
cd "$(dirname "$0")/.."
EXE_MOUNT=()
if [ -n "${ENSHROUDED_EXE:-}" ]; then EXE_MOUNT=(-v "$(realpath "$ENSHROUDED_EXE")":/exe/enshrouded_server.exe:ro -e ENSHROUDED_EXE=/exe/enshrouded_server.exe); fi
docker run --rm -v "$PWD":/p "${EXE_MOUNT[@]}" gcc:14 sh -c 'cd /p && g++ -std=c++17 -Itests/shim -Isrc -include cstdarg tests/state_test.cpp src/state.cpp src/common.cpp -o /tmp/t && /tmp/t && g++ -std=c++17 -Isrc tests/moderation_test.cpp -o /tmp/m && /tmp/m'
