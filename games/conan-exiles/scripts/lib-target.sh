#!/usr/bin/env bash
# The catalog target every Conan Exiles script builds against.
#
# Source it, do not run it. `conan_resolve_target [target-id]` exports the CONAN_EXILES_* keys
# `takaro-maint targets resolve` produces, so no script here hard-codes a game build, a
# toolchain, an image digest, a dependency URL or an artifact name.
#
# The resolution itself is `scripts/lib/target.sh`, shared by every game; these are the
# Conan Exiles names for it, so nothing that sources this file has to change.

# shellcheck source=../../../scripts/lib/target.sh
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"

conan_repo_root() {
    takaro_repo_root
}

conan_resolve_target() {
    takaro_resolve_target conan-exiles CONAN_EXILES "${1:-}"
}

# The one flag every Conan Exiles script takes. Exports TARGET (empty = the game's default target).
conan_parse_target_flag() {
    takaro_parse_target_flag "$@"
}
