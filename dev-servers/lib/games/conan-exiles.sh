#!/usr/bin/env bash
# Conan Exiles: an exactly pinned Steam build; the native connector is built and unpacked
# beside it, but the rig does not preload it (see games/conan-exiles/DEVELOPMENT.md).
#
# Sourced by dev-servers/lib/common.sh. It registers the game in the shared registry and
# defines the steps install.sh, deploy-connector.sh and verify-connectors.sh dispatch to.

ds_register 120 'conan-exiles|conan-exiles.yml|-|conan-exiles|12|6|plugin|Conan Exiles (Linux, app 443030); native Takaro LD_PRELOAD connector built, loaded by hand|conan-exiles'

# The rig id contains a hyphen that is part of the catalog game id.
ds_target_game_conan_exiles() { printf 'conan-exiles'; }

# The compose file mounts the install at /conan from here, and that is what the ledger
# describes.
ds_target_dest_conan_exiles() { printf '%s/server' "$(ds_data_dir conan-exiles)"; }

install_conan_exiles() {
    local target dest
    target="$(ds_target conan-exiles)"
    dest="$(ds_target_dest conan-exiles)"

    ds_info "Resolving the catalog target for conan-exiles..."
    ds_write_target_env conan-exiles
    mkdir -p "$dest"

    # The exact pinned build, by depot manifest: the game's Linux content and the
    # Steamworks redistributable it needs. No Steam client, no updater, no branch head.
    ds_info "Installing the pinned Conan Exiles build (${target}) into ${dest}..."
    ds_maint install --game conan-exiles --target "$target" --dest "$dest"

    ds_info "Pulling the pinned image..."
    ds_compose conan-exiles pull
    mkdir -p "${dest}/ConanSandbox/Saved/Logs"

    "${DS_DIR}/scripts/deploy-connector.sh" conan-exiles
}

deploy_conan_exiles() {
    local target tmp
    target="$(ds_target conan-exiles)"
    ds_scratch_dir tmp

    ds_info "Building the Conan Exiles native connector for ${target} (pinned toolchain image)..."
    ds_maint build --game conan-exiles --target "$target" \
        --version "$("${REPO_ROOT}/scripts/dev-version.sh" conan-exiles)" \
        --out "$tmp"
    ds_maint deploy --game conan-exiles --target "$target" \
        --dest "$(ds_target_dest conan-exiles)" \
        --from "${tmp}/build-manifest.json"
    ds_ok "$(ds_target_dest conan-exiles)/TakaroConanNative/TakaroConanNative"
}

ds_source_paths_conan_exiles() { echo "games/conan-exiles/native/core games/conan-exiles/native/platform games/conan-exiles/scripts games/conan-exiles/version.txt catalog/conan-exiles"; }
# Only once the library is preloaded by hand; it logs to its own file, not to the container.
ds_success_pattern_conan_exiles() { echo "takaro: identified"; }
ds_connector_logs_conan_exiles() { tail -n 2000 "$(ds_target_dest conan-exiles)/ConanSandbox/Saved/Logs/TakaroConanNative.log" 2>/dev/null; }
