#!/usr/bin/env bash
set -euo pipefail

plugin_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${STARRUPTURE_NATIVE_TEST_BUILD_DIR:-${TMPDIR:-/tmp}/starrupture-native-tests}"
mkdir -p "$build_root"


"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$plugin_root/src" \
  "$plugin_root/tests/starrupture_world_phase_test.cpp" \
  -o "$build_root/starrupture_world_phase_test"

"$build_root/starrupture_world_phase_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -pthread \
  -I"$plugin_root/src" \
  "$plugin_root/src/plugin_state.cpp" \
  "$plugin_root/tests/plugin_state_test.cpp" \
  -o "$build_root/plugin_state_test"

"$build_root/plugin_state_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$plugin_root/src" \
  "$plugin_root/src/plugin_lifecycle.cpp" \
  "$plugin_root/tests/plugin_lifecycle_test.cpp" \
  -o "$build_root/plugin_lifecycle_test"

"$build_root/plugin_lifecycle_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -pthread \
  -I"$plugin_root/src" \
  "$plugin_root/src/plugin_state.cpp" \
  "$plugin_root/src/starrupture_admin.cpp" \
  "$plugin_root/src/starrupture_catalogs.cpp" \
  "$plugin_root/src/starrupture_items.cpp" \
  "$plugin_root/src/starrupture_moderation.cpp" \
  "$plugin_root/src/starrupture_player_actions.cpp" \
  "$plugin_root/src/starrupture_sdk_bridge.cpp" \
  "$plugin_root/tests/starrupture_sdk_bridge_test.cpp" \
  -o "$build_root/starrupture_sdk_bridge_test"

"$build_root/starrupture_sdk_bridge_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$plugin_root/src" \
  "$plugin_root/src/starrupture_admin.cpp" \
  "$plugin_root/tests/starrupture_admin_test.cpp" \
  -o "$build_root/starrupture_admin_test"

"$build_root/starrupture_admin_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$plugin_root/src" \
  "$plugin_root/src/starrupture_catalogs.cpp" \
  "$plugin_root/src/starrupture_items.cpp" \
  "$plugin_root/tests/starrupture_catalogs_test.cpp" \
  -o "$build_root/starrupture_catalogs_test"

"$build_root/starrupture_catalogs_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$plugin_root/src" \
  "$plugin_root/src/modloader_process_events.cpp" \
  "$plugin_root/src/starrupture_chat_hooks.cpp" \
  "$plugin_root/tests/starrupture_chat_hooks_test.cpp" \
  -o "$build_root/starrupture_chat_hooks_test"

"$build_root/starrupture_chat_hooks_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$plugin_root/src" \
  "$plugin_root/src/starrupture_combat_events.cpp" \
  "$plugin_root/tests/starrupture_combat_events_test.cpp" \
  -o "$build_root/starrupture_combat_events_test"

"$build_root/starrupture_combat_events_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$plugin_root/src" \
  "$plugin_root/src/starrupture_items.cpp" \
  "$plugin_root/tests/starrupture_items_test.cpp" \
  -o "$build_root/starrupture_items_test"

"$build_root/starrupture_items_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$plugin_root/src" \
  "$plugin_root/src/starrupture_moderation.cpp" \
  "$plugin_root/tests/starrupture_moderation_test.cpp" \
  -o "$build_root/starrupture_moderation_test"

"$build_root/starrupture_moderation_test"

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$plugin_root/src" \
  "$plugin_root/src/starrupture_player_actions.cpp" \
  "$plugin_root/tests/starrupture_player_actions_test.cpp" \
  -o "$build_root/starrupture_player_actions_test"

"$build_root/starrupture_player_actions_test"


"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -pthread \
  -I"$plugin_root/src" \
  "$plugin_root/src/plugin_state.cpp" \
  "$plugin_root/src/takaro_protocol.cpp" \
  "$plugin_root/tests/takaro_protocol_test.cpp" \
  -o "$build_root/takaro_protocol_test"

"$build_root/takaro_protocol_test"
