#!/usr/bin/env bash
# Per-game runtime config rendering. Sourced by install.sh and reconfigure.sh so
# there is exactly one implementation of "write this game's Takaro config".
#
# Rust and Minecraft are absent on purpose: their connectors read environment
# variables that compose already supplies, so they have no file to render.

# Renders the Takaro config for one game. Safe to re-run.
ds_render_config() {
    local game="$1" data
    # Re-read .env: a long install may have started before the user pasted their
    # token, and the values loaded at process start would then be stale.
    ds_load_env
    data="$(ds_data_dir "$game")"
    export TAKARO_WS_URL="${TAKARO_WS_URL:-wss://connect.takaro.io/}"

    case "$game" in
        7d2d)
            export IDENTITY_TOKEN="${TAKARO_IDENTITY_7D2D:-takaro-dev-7d2d}"
            ds_render "${DS_TEMPLATES}/7d2d/Config.xml" \
                "${data}/ServerFiles/Takaro/Config.xml"
            ds_ok "${data}/ServerFiles/Takaro/Config.xml"
            ;;
        valheim)
            export IDENTITY_TOKEN="${TAKARO_IDENTITY_VALHEIM:-takaro-dev-valheim}"
            export SERVER_NAME="${VALHEIM_SERVER_NAME:-Takaro Dev Valheim}"
            export CHAT_SENDER_NAME="${VALHEIM_CHAT_SENDER_NAME:-Takaro}"
            export LOG_LEVEL="${VALHEIM_LOG_LEVEL:-Information}"
            ds_render "${DS_TEMPLATES}/valheim/com.takaro.valheim.cfg" \
                "${data}/config/bepinex/com.takaro.valheim.cfg"
            ds_ok "${data}/config/bepinex/com.takaro.valheim.cfg (chatSenderName=${CHAT_SENDER_NAME})"
            ;;
        conan-exiles)
            export IDENTITY_TOKEN="${TAKARO_IDENTITY_CONAN:-takaro-dev-conan}"
            export SERVER_NAME="${CONAN_SERVER_NAME:-Takaro Dev Conan}"
            mkdir -p "${data}/bridge"
            ds_render "${DS_TEMPLATES}/conan-exiles/TakaroConfig.txt" \
                "${data}/bridge/TakaroConfig.txt"
            ds_ok "${data}/bridge/TakaroConfig.txt"
            ;;
        palworld)
            export SERVER_NAME="${PALWORLD_SERVER_NAME:-Takaro Dev Palworld}"
            export ADMIN_PASSWORD="${ADMIN_PASSWORD:?set ADMIN_PASSWORD in dev-servers/.env}"
            mkdir -p "${data}/bridge"
            # This path is a bind-mounted FILE; Docker would silently create a
            # directory in its place if it did not already exist.
            ds_render "${DS_TEMPLATES}/palworld/TakaroConfig.txt" \
                "${data}/bridge/TakaroConfig.txt"
            ds_ok "${data}/bridge/TakaroConfig.txt"
            ;;
        terraria)
            ds_render_terraria_rest "$data"
            ;;
        rust|minecraft-*|zomboid)
            ds_info "${game} is configured through environment variables — nothing to render"
            ;;
        *)
            ds_die "no config renderer for ${game}"
            ;;
    esac
}

# Takaro drives Terraria over the TShock REST API, not the plugin. Merge our
# settings into TShock's own generated config.json rather than replacing it, so
# this survives TShock schema changes.
ds_render_terraria_rest() {
    local data="$1" config="${1}/tshock/config.json" patch

    if [ ! -f "$config" ]; then
        ds_warn "TShock has not generated ${config} yet — start terraria once, then re-run"
        return 0
    fi

    if [ -z "${TERRARIA_REST_TOKEN:-}" ]; then
        TERRARIA_REST_TOKEN="$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')"
        export TERRARIA_REST_TOKEN
        # Persist it so re-running this never rotates the token out from under
        # a server already registered in the Takaro dashboard.
        ds_persist_env TERRARIA_REST_TOKEN "$TERRARIA_REST_TOKEN"
        ds_info "Generated a TShock REST token and saved it to dev-servers/.env"
        printf '    TERRARIA_REST_TOKEN=%s\n' "$TERRARIA_REST_TOKEN"
    fi
    export SERVER_NAME="${TERRARIA_WORLD_NAME:-TakaroDev}"
    export TERRARIA_MAXPLAYERS="${TERRARIA_MAXPLAYERS:-8}"

    patch="${data}/tshock/.takaro-rest-patch.json"
    ds_render "${DS_TEMPLATES}/terraria/rest-patch.json" "$patch"
    python3 - "$patch" "$config" <<'PY'
import json, sys
patch_path, config_path = sys.argv[1], sys.argv[2]
with open(patch_path) as fh:
    patch = json.load(fh)
patch.pop("_comment", None)
with open(config_path) as fh:
    config = json.load(fh)
for section, values in patch.items():
    config.setdefault(section, {}).update(values)
with open(config_path, "w") as fh:
    json.dump(config, fh, indent=2)
PY
    rm -f "$patch"
    ds_ok "${config} (REST enabled on 7878)"
}
