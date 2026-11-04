#!/usr/bin/env bash
# Empyrion - Galactic Survival: the Windows dedicated server under Wine, with the Takaro C# mod
# in Content/Mods/TakaroEmpyrion.
#
# Sourced by dev-servers/lib/common.sh. It registers the game in the shared registry and
# defines the steps install.sh and deploy-connector.sh dispatch to.
#
# The server install and the Wine prefix live outside dev-servers/ (EMPYRION_SERVER_DIR and
# EMPYRION_WINE_DIR, an 18 GB depot): compose/empyrion.yml bind-mounts both. The mod holds the
# Takaro connection itself, so there is one container.
ds_register 140 'empyrion|empyrion.yml|-|empyrion|8|25|mod|Empyrion - Galactic Survival (Wine, app 530870) + Takaro C# mod|empyrion'

EMPYRION_SRC="${EMPYRION_SRC:-${REPO_ROOT}/games/empyrion}"
EMPYRION_RIG_LOCK="${EMPYRION_RIG_LOCK:-${DS_DATA}/empyrion-rig.lock}"

empyrion_server_dir() { printf '%s\n' "${EMPYRION_SERVER_DIR:-/srv/servers/empyrion/server}"; }

install_empyrion() {
    local dest
    dest="$(empyrion_server_dir)"
    mkdir -p "$dest" "${EMPYRION_WINE_DIR:-/srv/servers/empyrion/wine}"

    ds_info "Installing the pinned Empyrion server (catalog default target) into ${dest}..."
    ds_maint install --game empyrion --dest "$dest"

    ds_info "Building the Wine runtime image..."
    ds_compose empyrion build empyrion

    if [ ! -f "${dest}/${EMPYRION_DEDICATED_YAML:-dedicated-takaro.yaml}" ]; then
        ds_warn "no ${EMPYRION_DEDICATED_YAML:-dedicated-takaro.yaml} in ${dest}: copy dedicated.yaml to it and set
  Tel_Enabled: true, Tel_Port: 30004 and a Tel_Pwd before the first start (console output needs telnet)."
    fi

    "${DS_DIR}/scripts/deploy-connector.sh" empyrion
}

deploy_empyrion() {
    local dest mods
    dest="$(empyrion_server_dir)"
    mods="${dest}/Content/Mods"
    [ -d "$mods" ] || ds_die "${mods} does not exist; install the server first (dev-servers/scripts/install.sh empyrion)"

    ds_info "Building TakaroEmpyrion.dll (pinned mono image, pinned reference assemblies)..."
    "${EMPYRION_SRC}/scripts/build.sh"

    # Mono loads code lazily: a DLL swapped under a running server fails on the next method it
    # loads (BadImageFormatException). Swap it with the server stopped, under the rig lock.
    # TakaroConfig.txt, players.json, bans.json and Logs/ are not in the build and stay as they are.
    mkdir -p "$(dirname "$EMPYRION_RIG_LOCK")"
    local was_running=0
    ds_is_running empyrion && was_running=1
    flock -w 900 "$EMPYRION_RIG_LOCK" -c "
        set -e
        [ '${was_running}' = 1 ] && '${DS_DIR}/scripts/stop.sh' empyrion
        mkdir -p '${mods}/TakaroEmpyrion'
        cp '${EMPYRION_SRC}/bin/TakaroEmpyrion/TakaroEmpyrion.dll' \
           '${EMPYRION_SRC}/bin/TakaroEmpyrion/TakaroEmpyrion_Info.yaml' \
           '${EMPYRION_SRC}/bin/TakaroEmpyrion/TakaroConfig.txt.example' '${mods}/TakaroEmpyrion/'
        [ '${was_running}' = 1 ] && '${DS_DIR}/scripts/start.sh' empyrion
        true
    " || ds_die "mod deploy under the rig lock failed"
    [ -f "${mods}/TakaroEmpyrion/TakaroConfig.txt" ] \
        || ds_warn "no ${mods}/TakaroEmpyrion/TakaroConfig.txt yet: copy TakaroConfig.txt.example and set REGISTRATION_TOKEN"
    ds_ok "${mods}/TakaroEmpyrion/TakaroEmpyrion.dll"
}

ds_source_paths_empyrion() { echo "games/empyrion/mod games/empyrion/TakaroConfig.txt.example games/empyrion/version.txt"; }

# The mod writes its own log inside the server tree, not to the container output.
ds_success_pattern_empyrion() { echo "identified as game server"; }
ds_connector_logs_empyrion() { tail -n 2000 "$(empyrion_server_dir)/Content/Mods/TakaroEmpyrion/Logs/takaro.log" 2>/dev/null; }
