#!/usr/bin/env bash
# The catalog target every Dragonwilds script builds against.
#
# Source it, do not run it. `dragonwilds_resolve_target [target-id]` exports the
# DRAGONWILDS_* keys `takaro-maint targets resolve` produces, so no script here hard-codes a
# server build, a toolchain image, a dependency URL or an artifact name.
#
# The resolution itself is `scripts/lib/target.sh`, shared by every game; these are the
# Dragonwilds names for it, so nothing that sources this file has to change.

# shellcheck source=../../../scripts/lib/target.sh
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"

dragonwilds_repo_root() {
    takaro_repo_root
}

dragonwilds_resolve_target() {
    takaro_resolve_target dragonwilds DRAGONWILDS "${1:-}"
}

# The one flag every Dragonwilds script takes. Exports TARGET (empty = the game's default target).
dragonwilds_parse_target_flag() {
    takaro_parse_target_flag "$@"
}
