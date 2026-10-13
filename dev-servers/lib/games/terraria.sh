#!/usr/bin/env bash
# Terraria: the exactly pinned TShock image, the Takaro events plugin and the Node bridge.
#
# Sourced by dev-servers/lib/common.sh. It registers the game in the shared registry and
# defines the steps install.sh, deploy-connector.sh and verify-connectors.sh dispatch to.
#
# Both connector roles come out of the catalog target: `takaro-maint install` puts the
# pinned distribution and the ledger under _data/terraria, `takaro-maint build` builds the
# plugin and the bridge for that target, and `takaro-maint deploy` unpacks them. The
# compose file reads the image and the bridge runtime from _data/.targets/terraria.env, so
# the rig, CI and a release all run the same bytes.

ds_register 10 'terraria|terraria.yml|-|terraria terraria-bridge|1|1|plugin|TShock server + Takaro events plugin (Takaro connects over TShock REST)|terraria'

install_terraria() {
    local target dest
    target="$(ds_target terraria)"
    dest="$(ds_target_dest terraria)"

    ds_info "Resolving the catalog target for terraria..."
    ds_write_target_env terraria
    mkdir -p "${dest}/tshock" "${dest}/tshock/takaro" "${dest}/worlds" "${dest}/plugins" "${dest}/bridge"

    ds_info "Installing the pinned TShock distribution (${target}) into ${dest}..."
    ds_maint install --game terraria --target "$target" --dest "$dest"

    ds_info "Pulling the pinned TShock image..."
    ds_compose terraria pull

    # First boot writes TShock's own config.json, which the REST patch below merges into.
    # Only the server: the bridge has nothing to talk to until that file exists.
    ds_info "First boot: generating TShock config and the world..."
    ds_compose terraria up -d terraria
    ds_wait_for_file terraria "${dest}/tshock/config.json" 900 "TShock config" \
        || ds_die "TShock never wrote config.json; check 'dev-servers/scripts/logs.sh terraria'"
    cleanup_stop
    ds_fix_ownership "$DATA"

    # Takaro drives Terraria over the TShock REST API; this enables it and mints the token.
    ds_render_config terraria
    ds_render_terraria_bridge_config "$dest"

    "${DS_DIR}/scripts/deploy-connector.sh" terraria
}

# The bridge's only configuration. Rendered here rather than from dev-servers/templates
# because the file is this game's own shape and its two secrets come from .env.
ds_render_terraria_bridge_config() {
    local dest="$1" config="${1}/bridge/TakaroConfig.txt"
    ds_load_env
    mkdir -p "${dest}/bridge"
    # This path is a bind-mounted FILE; Docker would silently create a directory in its
    # place if it did not already exist.
    umask 077
    cat > "$config" <<EOF
# Rendered by dev-servers/scripts/install.sh. Holds live tokens: do not commit it.
registrationToken=${TAKARO_REGISTRATION_TOKEN}
identityToken=${TAKARO_IDENTITY_TERRARIA:-takaro-dev-terraria}
serverName=${SERVER_NAME:-Takaro Dev Terraria}
serverChatName=Takaro
takaroWsUrl=${TAKARO_WS_URL:-wss://connect.takaro.io/}
tshockBaseUrl=http://127.0.0.1:7878
tshockToken=${TERRARIA_REST_TOKEN}
httpPort=3020
pollIntervalMs=5000
logFiles=/tshock/logs
mapDir=/tshock/takaro
commandAllowlistExact=help,/help
commandAllowlistPrefixes=say,time
enableShutdown=false
EOF
    chmod 600 "$config"
    ds_ok "${config}"
}

deploy_terraria() {
    local target tmp
    target="$(ds_target terraria)"
    ds_scratch_dir tmp

    ds_info "Building the Terraria plugin and bridge for ${target} (pinned .NET SDK and Node images)..."
    ds_maint build --game terraria --target "$target" \
        --version "$("${REPO_ROOT}/scripts/dev-version.sh" terraria)" \
        --out "$tmp"
    ds_maint deploy --game terraria --target "$target" \
        --dest "$(ds_target_dest terraria)" \
        --from "${tmp}/build-manifest.json"
    ds_ok "$(ds_target_dest terraria)/plugins/TakaroTerrariaEvents.dll"
    ds_ok "$(ds_target_dest terraria)/bridge/TakaroTerrariaBridge"
}

ds_source_paths_terraria() { echo "games/terraria/mod/src games/terraria/version.txt"; }
# The bridge, not the plugin, is what holds the websocket, so this is the line it writes.
ds_success_pattern_terraria() { echo "Identified successfully"; }
