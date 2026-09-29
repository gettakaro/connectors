#!/usr/bin/env bash
# Host-side tests with a tiny windows.h shim. Needs docker.
#   state_test: log-line correlator (state.cpp)
#   names_test: item/entity/location display names (names.cpp), vectors plus the whole gamedata table
#   moderation_test: canKickBan admin-protection bypass helpers; set ENSHROUDED_EXE=<path to enshrouded_server.exe>
#                    to also check the anchors against the pinned binary.
#   native_test: the direct Takaro connector (src/native/): parity with the former sidecar (tests/fixtures,
#                produced by sidecar `npm run parity-fixtures`), bridge behind a loopback transport, outbox,
#                timed bans, persistence, heartbeat. NATIVE_ONLY=1 runs just this one.
set -euo pipefail
cd "$(dirname "$0")/.."
EXE_MOUNT=()
if [ -n "${ENSHROUDED_EXE:-}" ]; then EXE_MOUNT=(-v "$(realpath "$ENSHROUDED_EXE")":/exe/enshrouded_server.exe:ro -e ENSHROUDED_EXE=/exe/enshrouded_server.exe); fi
NATIVE_SRC="src/native/json_util.cpp src/native/mapping.cpp src/native/adapter.cpp src/native/fileio.cpp src/native/persistence.cpp src/native/logtail.cpp src/native/transport.cpp src/native/config.cpp src/native/bridge.cpp src/common.cpp"
NATIVE="g++ -std=c++17 -O1 -pthread -Wall -Wno-unused-function -Itests/shim -Isrc -include cstdarg tests/native/*.cpp $NATIVE_SRC -o /tmp/native && /tmp/native tests/fixtures"
LEGACY="g++ -std=c++17 -Itests/shim -Isrc -include cstdarg tests/state_test.cpp src/state.cpp src/common.cpp -o /tmp/t && /tmp/t && g++ -std=c++17 -Isrc tests/moderation_test.cpp -o /tmp/m && /tmp/m && g++ -std=c++17 -Isrc tests/names_test.cpp src/names.cpp src/gamedata.cpp -o /tmp/n && /tmp/n"
if [ -n "${NATIVE_ONLY:-}" ]; then CMD="$NATIVE"; else CMD="$LEGACY && $NATIVE"; fi
docker run --rm -v "$PWD":/p "${EXE_MOUNT[@]}" gcc:14@sha256:cb57ac6c7917425c057c736fe3a240df25bce310418d61cd223bc8e411364876 sh -c "cd /p && $CMD"
