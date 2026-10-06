#!/usr/bin/env bash
# The catalog target every 7D2D script builds against.
#
# Source it, do not run it. `sevend2d_resolve_target [target-id]` exports the SEVEND2D_* keys
# `takaro-maint targets resolve` produces, so no script here hard-codes a game build, a
# Mono/MSBuild toolchain, an image digest, a dependency URL or an artifact name.
#
# The resolution itself is `scripts/lib/target.sh`, shared by every game; these are the
# 7D2D names for it, so nothing that sources this file has to change.

# shellcheck source=../../../scripts/lib/target.sh
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"

sevend2d_repo_root() {
    takaro_repo_root
}

sevend2d_resolve_target() {
    takaro_resolve_target 7d2d SEVEND2D "${1:-}"
}

# The one flag every 7D2D script takes. Exports TARGET (empty = the game's default target).
sevend2d_parse_target_flag() {
    takaro_parse_target_flag "$@"
}

# The compile-time symbols for the game API the resolved target exposes. V3.3.0 moved the
# player-data inventory into serialised blobs and replaced ItemValue.Modifications with
# accessors, so the mod has one code path per API generation. Needs SEVEND2D_REVISION.
sevend2d_api_defines() {
    local major minor
    IFS=. read -r major minor _ <<<"${SEVEND2D_REVISION:?resolve the target first}"
    if [ "${major}" -gt 3 ] || { [ "${major}" -eq 3 ] && [ "${minor}" -ge 3 ]; }; then
        echo "SEVEND2D_V3_3"
    fi
}
