#!/usr/bin/env bash
# Installs one game: image, game files, runtime config, connector artifact.
# Leaves the game STOPPED. Use start.sh to run it.
#
# Usage: install.sh <game> [--force]
set -euo pipefail
DS_LIB="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../lib" && pwd)"
# shellcheck source=../lib/common.sh
. "${DS_LIB}/common.sh"
# shellcheck source=../lib/render.sh
. "${DS_LIB}/render.sh"

GAME="${1:?usage: install.sh <game> [--force]}"
FORCE="${2:-}"
ds_validate_game "$GAME"

ds_load_env
# Installing is mostly downloading, which does not need credentials. If the
# token is missing the configs are still rendered (with a blank token) and the
# game is flagged so reconfigure.sh can fill it in later.
if [ -z "${TAKARO_REGISTRATION_TOKEN:-}" ]; then
    ds_warn "TAKARO_REGISTRATION_TOKEN is empty — installing anyway."
    ds_warn "Set it in dev-servers/.env, then run: dev-servers/scripts/reconfigure.sh"
    touch "${DS_DATA}/.needs-reconfigure" 2>/dev/null || true
fi

if ds_is_installed "$GAME" && [ "$FORCE" != "--force" ]; then
    ds_info "${GAME} is already installed. Use --force to reinstall."
    exit 0
fi

DATA="$(ds_data_dir "$GAME")"
mkdir -p "$DATA" "$(dirname "$(ds_marker "$GAME")")"

# Every server gets its own identity token so one Takaro organisation can hold
# all of them at once. Exported for template rendering.
export TAKARO_WS_URL="${TAKARO_WS_URL:-wss://connect.takaro.io/}"

cleanup_stop() {
    ds_info "Stopping ${GAME} (install leaves games stopped)"
    ds_compose "$GAME" stop >/dev/null 2>&1 || true
}

# ── Per-game install steps ───────────────────────────────────────────────────

install_rust() {
    ds_info "Building the Rust server image (SteamCMD + Carbon, ~12 GB, slow)..."
    ds_compose rust build
    mkdir -p "${DATA}/plugins" "${DATA}/server" "${DATA}/carbon-logs"
    # Rust's connector is configured purely through environment variables, so
    # there is no config file to render.
    "${DS_DIR}/scripts/deploy-connector.sh" rust
}

install_minecraft() {
    local platform="$1"
    mkdir -p "$DATA"

    if [ -n "$(ds_target "$GAME")" ]; then
        # Catalog-driven: the exact server jar, loader launcher and API jar the
        # connector was built against, verified by hash and recorded in a ledger.
        ds_info "Resolving the catalog target for ${GAME}..."
        ds_write_target_env "$GAME"
        ds_info "Installing pinned server files into ${DATA}..."
        ds_maint install --game minecraft --target "$(ds_target "$GAME")" --dest "$DATA"
    fi

    ds_info "Pulling the Minecraft server image..."
    ds_compose "$GAME" pull "$platform"
    # Minecraft env vars override the connector's file config, and compose
    # already supplies them, so there is nothing to render here either.
    "${DS_DIR}/scripts/deploy-connector.sh" "$GAME"
}

install_dayz() {
    local DATA="${DS_DATA}/dayz"
    mkdir -p "${DATA}/server" "${DATA}/profiles/TakaroIntegration" "${DATA}/bridge" "${DATA}/mod"

    # DayZ is the one game whose server files this rig cannot fetch itself: Steam
    # app 223350 refuses `login anonymous` (Bohemia T179224), so the Linux depot is
    # downloaded elsewhere with an account that owns DayZ and dropped into _data.
    if [ ! -f "${DATA}/server/DayZServer" ]; then
        ds_die "DayZ server files are missing from ${DATA}/server/.
  Steam app 223350 cannot be downloaded anonymously. Fetch the Linux depot with an
  account that owns DayZ and place it in ${DATA}/server/ (DayZServer must be at the top),
  or set STEAM_USER on the dayz service and let images/dayz/entrypoint.sh run SteamCMD."
    fi

    local mod_dir="${DAYZ_MOD_DIR:-../_data/dayz/mod/@takarointegration}"
    case "$mod_dir" in ../*) mod_dir="${DS_COMPOSE_DIR}/${mod_dir}" ;; esac
    [ -d "$mod_dir" ] || ds_warn "mod folder ${mod_dir} does not exist yet (DAYZ_MOD_DIR); the server will start without the Takaro mod."

    ds_info "Building the DayZ server image and the Takaro sidecar image..."
    ds_compose dayz build

    ds_fix_ownership "${DATA}"
}

install_zomboid() {
    ds_info "Pulling the Project Zomboid server image..."
    ds_compose zomboid pull
    mkdir -p "${DATA}/config/Takaro" "${DATA}/server"
    chmod -R 777 "$DATA" 2>/dev/null || true

    ds_info "First boot: downloading Project Zomboid B42 server files (SteamCMD 380870)..."
    ds_compose zomboid up -d
    # The agent compiles against the game jar; wait for SteamCMD to place it.
    ds_wait_for_condition zomboid \
        "[ -r '${DATA}/server/java/projectzomboid.jar' ]" \
        3600 "Project Zomboid server jar" \
        || ds_die "Zomboid server files never appeared; check 'dev-servers/scripts/logs.sh zomboid'"
    cleanup_stop
    ds_fix_ownership "$DATA"

    # The javaagent reads its config from compose env vars (TAKARO_*); there is
    # no file to render (an optional Zomboid/Takaro/TakaroConfig.txt can still
    # override, env wins).
    "${DS_DIR}/scripts/deploy-connector.sh" zomboid
}

install_7d2d() {
    ds_info "Pulling the 7 Days to Die server image..."
    ds_compose 7d2d pull
    mkdir -p "${DATA}/ServerFiles" "${DATA}/7DaysToDie" "${DATA}/log"
    chmod -R 777 "$DATA"

    ds_info "First boot: downloading 7D2D server files (~15 GB)..."
    ds_compose 7d2d up -d
    # Wait for SteamCMD to FINALISE the install, not merely to write the binary.
    # Stopping at first-file-seen leaves StateFlags at "update required", and
    # LinuxGSM then refuses to start the server on every subsequent boot.
    ds_wait_for_condition 7d2d \
        "ds_steam_app_ready '${DATA}/ServerFiles/steamapps/appmanifest_294420.acf'" \
        5400 "7D2D server files (fully installed)" \
        || ds_die "7D2D install never completed; check 'dev-servers/scripts/logs.sh 7d2d'"
    cleanup_stop
    ds_fix_ownership "$DATA"

    # The mod reads <server cwd>/Takaro/Config.xml and has no env-var config.
    ds_render_config 7d2d

    "${DS_DIR}/scripts/deploy-connector.sh" 7d2d
}

install_valheim() {
    ds_info "Pulling the Valheim server image..."
    ds_compose valheim pull
    mkdir -p "${DATA}/config" "${DATA}/server"

    ds_info "First boot: downloading Valheim and installing BepInEx..."
    ds_compose valheim up -d
    ds_wait_for_file valheim "${DATA}/config/bepinex/BepInEx.cfg" 1800 "BepInEx install" \
        || ds_die "BepInEx never installed; check 'dev-servers/scripts/logs.sh valheim'"
    cleanup_stop
    ds_fix_ownership "$DATA"

    ds_render_config valheim

    "${DS_DIR}/scripts/deploy-connector.sh" valheim
}

install_terraria() {
    ds_info "Pulling the TShock image..."
    ds_compose terraria pull
    mkdir -p "${DATA}/tshock" "${DATA}/worlds" "${DATA}/plugins"
    chmod -R 777 "${DATA}"

    ds_info "First boot: generating TShock config and the world..."
    ds_compose terraria up -d
    ds_wait_for_file terraria "${DATA}/tshock/config.json" 900 "TShock config" \
        || ds_die "TShock never wrote config.json; check 'dev-servers/scripts/logs.sh terraria'"
    cleanup_stop
    ds_fix_ownership "$DATA"

    # Takaro drives Terraria over the TShock REST API, not the plugin.
    ds_render_config terraria

    "${DS_DIR}/scripts/deploy-connector.sh" terraria
}

install_conan() {
    ds_info "Building the Conan Exiles server image..."
    ds_compose conan-exiles build conan-exiles
    mkdir -p "${DATA}/server" "${DATA}/bridge" "${DATA}/logs"

    ds_info "Downloading Conan Exiles game files (~35 GB, this takes a long time)..."
    # CONAN_INSTALL_ONLY makes the entrypoint exit after SteamCMD instead of
    # launching the server, so install stays a foreground, one-shot step.
    ds_compose conan-exiles run --rm \
        -e CONAN_INSTALL_ONLY=1 --no-deps conan-exiles
    ds_fix_ownership "$DATA"

    ds_render_config conan-exiles

    mkdir -p "${DATA}/server/ConanSandbox/Saved/Logs"
    "${DS_DIR}/scripts/deploy-connector.sh" conan-exiles
}

install_palworld() {
    ds_info "Pulling the Palworld server image..."
    ds_compose palworld pull palworld
    mkdir -p "${DATA}/server" "${DATA}/bridge"

    # Render the bridge config BEFORE any compose up: this path is a bind-mounted
    # FILE, and Docker would silently create a directory in its place if missing.
    ds_render_config palworld

    "${DS_DIR}/scripts/deploy-connector.sh" palworld

    ds_info "First boot: downloading Palworld server files (~10 GB)..."
    ds_compose palworld up -d palworld
    ds_wait_for_file palworld "${DATA}/server/Pal" 2400 "Palworld server files" \
        || ds_die "Palworld server files never appeared; check 'dev-servers/scripts/logs.sh palworld'"
    cleanup_stop
    ds_fix_ownership "$DATA"
}

install_dragonwilds() {
    # The dev rig uses its own project name, container and data dir
    # (takaro-dev-dragonwilds / _data/dragonwilds-dev).
    mkdir -p "${DS_DATA}/dragonwilds-dev" "${DS_DATA}/dragonwilds-plugin" "${DS_DATA}/dragonwilds-sidecar"

    ds_info "Building the patched Dragonwilds server image (LD_PRELOAD on the game binary only)..."
    ds_compose dragonwilds build dragonwilds

    ds_info "First boot: downloading Dragonwilds server files (SteamCMD app 4019830, ~5.2 GB)..."
    ds_info "The first SteamCMD attempt often fails with 'Missing configuration'; the wrapper retries."
    ds_compose dragonwilds up -d dragonwilds

    # The .sym file (301 MB) is what the Takaro plugin resolves engine functions from, so it
    # is part of "installed", not an optional extra. Wait for the binary AND its symbols.
    local bin="${DS_DATA}/dragonwilds-dev/RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping"
    ds_wait_for_condition dragonwilds \
        "[ -s '${bin}' ] && [ -s '${bin}.sym' ]" \
        5400 "Dragonwilds server binary + .sym" \
        || ds_die "Dragonwilds server files never appeared; check 'dev-servers/scripts/logs.sh dragonwilds'"
    # SteamCMD writes the binary before it finalises the manifest; wait for StateFlags 4 too.
    ds_wait_for_condition dragonwilds \
        "ds_steam_app_ready '${DS_DATA}/dragonwilds-dev/steamapps/appmanifest_4019830.acf'" \
        1800 "Dragonwilds install manifest (fully installed)" \
        || ds_warn "appmanifest never reached StateFlags 4 — the server may re-validate on next boot"

    ds_fix_ownership "${DS_DATA}/dragonwilds-dev"

    # The plugin is built and deployed separately (deploy-connector.sh dragonwilds); it is a
    # no-op until the connector source tree exists.
    "${DS_DIR}/scripts/deploy-connector.sh" dragonwilds
}

# ── Run ──────────────────────────────────────────────────────────────────────

ds_info "Installing ${GAME} — $(ds_description "$GAME")"
ds_info "Estimated download/disk: ~$(ds_disk_gb "$GAME") GB; free now: $(ds_free_disk_gb) GB"

case "$GAME" in
    rust)               install_rust ;;
    minecraft-paper)    install_minecraft paper ;;
    minecraft-neoforge) install_minecraft neoforge ;;
    minecraft-fabric)   install_minecraft fabric ;;
    minecraft-fabric-26.1.2) install_minecraft fabric-26-1-2 ;;
    7d2d)               install_7d2d ;;
    zomboid)            install_zomboid ;;
    dayz)               install_dayz ;;
    dragonwilds)        install_dragonwilds ;;
    valheim)            install_valheim ;;
    terraria)           install_terraria ;;
    conan-exiles)       install_conan ;;
    palworld)           install_palworld ;;
    *)                  ds_die "no install step defined for ${GAME}" ;;
esac

cleanup_stop
date -Iseconds > "$(ds_marker "$GAME")"
ds_ok "${GAME} installed and stopped. Start it with: dev-servers/scripts/start.sh ${GAME}"
