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

# The builder image: the four static libraries the plugin links, by hashed source archive, on
# the pinned Bookworm base, all from the catalog. Tagged with the target fingerprint, so a
# re-pinned target builds its own image.
dragonwilds_builder_image() {
    local project_root
    project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
    BUILDER_IMAGE="takaro-dragonwilds-builder:${DRAGONWILDS_FP16:?resolve the target first}"
    docker build -q \
        -f "${project_root}/Dockerfile.builder" \
        --build-arg "TOOLCHAIN=${DRAGONWILDS_TOOLCHAIN:?resolve the target first}" \
        --build-arg "OPENSSL_URL=${DRAGONWILDS_DEP_OPENSSL_URL:?}" \
        --build-arg "OPENSSL_SHA256=${DRAGONWILDS_DEP_OPENSSL_SHA256:?}" \
        --build-arg "LIBWEBSOCKETS_URL=${DRAGONWILDS_DEP_LIBWEBSOCKETS_URL:?}" \
        --build-arg "LIBWEBSOCKETS_SHA256=${DRAGONWILDS_DEP_LIBWEBSOCKETS_SHA256:?}" \
        --build-arg "PCRE2_URL=${DRAGONWILDS_DEP_PCRE2_URL:?}" \
        --build-arg "PCRE2_SHA256=${DRAGONWILDS_DEP_PCRE2_SHA256:?}" \
        --build-arg "NLOHMANN_JSON_URL=${DRAGONWILDS_DEP_NLOHMANN_JSON_URL:?}" \
        --build-arg "NLOHMANN_JSON_SHA256=${DRAGONWILDS_DEP_NLOHMANN_JSON_SHA256:?}" \
        -t "${BUILDER_IMAGE}" \
        "${project_root}" >/dev/null || return 1
    export BUILDER_IMAGE
}
