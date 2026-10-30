#!/usr/bin/env bash
# Host-side tests with a tiny windows.h shim. Needs docker.
#   state_test: log-line correlator (state.cpp)
#   game_support_test: the unsupported-game-build notice (plugin.log, /health gameSupport, reachability, console)
#     and the /health "starting" list (hooks waiting for the game world)
#   state_login_test: login -> Steam account binding by named peer and Steam auth (1273043), unresolved when unsure
#   names_test: item/entity/location display names (names.cpp), vectors plus the whole gamedata table
#   weapon_test: entity-killed weapon names (weapon.cpp, itemlabels.cpp): item labels, category fallback, Unarmed
#   entity_names_test: entity names (entity.cpp, entitylabels.cpp): client, Logic Spawner, item and variant names,
#                      the listEntities and listItems filters
#   location_test: listLocations rows (location.cpp, locationlabels.cpp): unique codes, in-game map names
#   console_args_test: console command arguments (console_args.cpp): quoted and bare player names with spaces,
#                      SteamID64, refusal of unknown, shared and ambiguous names and of unclosed quotes,
#                      reply display names
#   moderation_test: canKickBan admin-protection bypass helpers; set ENSHROUDED_EXE=<path to enshrouded_server.exe>
#                    to also check the anchors against the pinned binary.
#   native_test: the direct Takaro connector (src/native/): parity with the former sidecar (tests/fixtures,
#                produced by sidecar `npm run parity-fixtures`), bridge behind a loopback transport, outbox,
#                timed bans, persistence, heartbeat, Game Profile (icon, identify, version = ../version.txt),
#                the takaro/enshrouded-gamedata.json upgrade (src/gamedata_file.cpp).
#                NATIVE_ONLY=1 runs just this one.
set -euo pipefail
cd "$(dirname "$0")/.."
EXE_MOUNT=()
if [ -n "${ENSHROUDED_EXE:-}" ]; then EXE_MOUNT=(-v "$(realpath "$ENSHROUDED_EXE")":/exe/enshrouded_server.exe:ro -e ENSHROUDED_EXE=/exe/enshrouded_server.exe); fi
NATIVE_SRC="src/native/json_util.cpp src/native/mapping.cpp src/native/adapter.cpp src/native/server_chat.cpp src/native/maptiles.cpp src/native/capabilities.cpp src/native/game_profile.cpp src/native/fileio.cpp src/native/persistence.cpp src/native/logtail.cpp src/native/transport.cpp src/native/config.cpp src/native/config_file.cpp src/native/bridge.cpp src/gamedata_file.cpp src/common.cpp"
NATIVE="g++ -std=c++17 -O1 -pthread -Wall -Wno-unused-function -Itests/shim -Isrc -include cstdarg tests/native/*.cpp $NATIVE_SRC -o /tmp/native && /tmp/native tests/fixtures"
LEGACY="g++ -std=c++17 -Itests/shim -Isrc -include cstdarg tests/state_test.cpp src/state.cpp src/common.cpp -o /tmp/t && /tmp/t && g++ -std=c++17 -Itests/shim -Isrc -include cstdarg tests/state_v1_test.cpp src/state.cpp src/common.cpp -o /tmp/t1 && /tmp/t1 && g++ -std=c++17 -Itests/shim -Isrc -include cstdarg tests/state_login_test.cpp src/state.cpp src/common.cpp -o /tmp/t2 && /tmp/t2 && g++ -std=c++17 -Itests/shim -Isrc -include cstdarg tests/game_support_test.cpp src/state.cpp src/common.cpp -o /tmp/gs && /tmp/gs && g++ -std=c++17 -Wall -Isrc tests/console_args_test.cpp src/console_args.cpp -o /tmp/c && /tmp/c && g++ -std=c++17 -Isrc tests/moderation_test.cpp -o /tmp/m && /tmp/m && g++ -std=c++17 -Isrc tests/names_test.cpp src/names.cpp src/gamedata.cpp -o /tmp/n && /tmp/n && g++ -std=c++17 -Isrc tests/weapon_test.cpp src/weapon.cpp src/itemlabels.cpp src/names.cpp src/gamedata.cpp -o /tmp/w && /tmp/w && g++ -std=c++17 -Isrc tests/entity_names_test.cpp src/entity.cpp src/entitylabels.cpp src/weapon.cpp src/itemlabels.cpp src/names.cpp src/gamedata.cpp -o /tmp/e && /tmp/e && g++ -std=c++17 -Isrc tests/location_test.cpp src/location.cpp src/locationlabels.cpp src/gamedata.cpp -o /tmp/l && /tmp/l"
# the compiled-in icon must be the committed PNG
python3 tools/gen_game_icon.py --check
if [ -n "${NATIVE_ONLY:-}" ]; then CMD="$NATIVE"; else CMD="$LEGACY && $NATIVE"; fi
docker run --rm -v "$PWD":/p -e ENSHROUDED_VERSION_TXT="$(cat ../version.txt)" "${EXE_MOUNT[@]}" gcc:14@sha256:cb57ac6c7917425c057c736fe3a240df25bce310418d61cd223bc8e411364876 sh -c "cd /p && $CMD"
