#!/usr/bin/env bash
# Enshrouded: the exactly pinned Proton image and the native Takaro connector in dbghelp.dll.
#
# Sourced by dev-servers/lib/common.sh. It registers the game in the shared registry and
# defines the steps install.sh, deploy-connector.sh and verify-connectors.sh dispatch to.
#
# Everything comes out of the catalog target: `takaro-maint install` lays the pinned server
# and its ledger down under _data/enshrouded/server, `takaro-maint build` builds the plugin
# for that target, and `takaro-maint deploy` places it under server/takaro/plugin/. The
# compose file reads the image from _data/.targets/enshrouded.env. The plugin holds the
# Takaro connection itself, so there is one container.
ds_register 65 'enshrouded|enshrouded.yml|-|enshrouded|6|40|plugin|Enshrouded (Proton, app 2278520) + native Takaro dbghelp.dll connector|enshrouded'

# takaro-maint installs the server one level below the game's data directory; the
# compose file mounts _data/enshrouded/server.
ds_target_dest_enshrouded() { printf '%s/server\n' "$(ds_data_dir enshrouded)"; }

ENSHROUDED_RIG_LOCK="${ENSHROUDED_RIG_LOCK:-${DS_DATA}/enshrouded-rig.lock}"

install_enshrouded() {
    local target dest
    target="$(ds_target enshrouded)"
    dest="$(ds_target_dest enshrouded)"

    ds_info "Resolving the catalog target for enshrouded..."
    ds_write_target_env enshrouded

    ds_info "Installing the pinned server (${target}) into ${dest}..."
    ds_maint install --game enshrouded --target "$target" --dest "$dest"
    ds_maint ledger check --game enshrouded --dest "$dest"

    ds_info "Pulling the pinned Proton image..."
    ds_compose enshrouded pull

    # compose refuses to start without the plugin (create_host_path: false on the DLL
    # bind), so deploy before the first boot.
    "${DS_DIR}/scripts/deploy-connector.sh" enshrouded
}

deploy_enshrouded() {
    local target tmp dest
    target="$(ds_target enshrouded)"
    dest="$(ds_target_dest enshrouded)"

    # A 0.5.0 sidecar still holding the same identity would fight the plugin for the
    # connection. games/enshrouded/INSTALL.md drains and removes it first.
    if docker container inspect takaro-dev-enshrouded-sidecar >/dev/null 2>&1; then
        ds_die "legacy Enshrouded sidecar container still exists; drain and remove it before a native deploy (games/enshrouded/INSTALL.md)"
    fi

    ds_scratch_dir tmp
    ds_info "Building the Enshrouded plugin for ${target} (pinned zig builder)..."
    ds_maint build --game enshrouded --target "$target" \
        --version "$("${REPO_ROOT}/scripts/dev-version.sh" enshrouded)" \
        --out "$tmp"

    # The DLL is bind-mounted read-only into a running game, so swap it with the game
    # stopped, under the rig lock that live tests also take.
    mkdir -p "$(dirname "$ENSHROUDED_RIG_LOCK")"
    local was_running=0
    ds_is_running enshrouded && was_running=1
    flock -w 900 "$ENSHROUDED_RIG_LOCK" -c "
        set -e
        [ '${was_running}' = 1 ] && '${DS_DIR}/scripts/stop.sh' enshrouded
        '${REPO_ROOT}/maintenance/bin/takaro-maint' deploy --game enshrouded --target '${target}' \
            --dest '${dest}' --from '${tmp}/build-manifest.json'
        [ '${was_running}' = 1 ] && '${DS_DIR}/scripts/start.sh' enshrouded
        true
    " || ds_die "plugin deploy under the rig lock failed"
    ds_ok "${dest}/takaro/plugin/dbghelp.dll"
}

ds_source_paths_enshrouded() { echo "games/enshrouded/mod/src games/enshrouded/version.txt"; }

# The plugin writes its own log inside the server tree, not to the container output.
ds_success_pattern_enshrouded() { echo "native: identified with Takaro"; }
ds_connector_logs_enshrouded() { tail -n 2000 "$(ds_target_dest enshrouded)/takaro/plugin.log" 2>/dev/null; }
