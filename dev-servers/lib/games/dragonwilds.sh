#!/usr/bin/env bash
# RuneScape: Dragonwilds — one native LD_PRELOAD connector in the game process.
#
# Sourced by dev-servers/lib/common.sh. It registers the game in the shared registry and
# defines the steps install.sh, deploy-connector.sh and verify-connectors.sh dispatch to.

ds_register 70 'dragonwilds|dragonwilds.yml|-|dragonwilds|4|8|plugin|RuneScape: Dragonwilds (Linux, app 4019830) + native Takaro LD_PRELOAD connector|dragonwilds-dev'

# The rig's own plugin and connector-state directories. Not `_data/dragonwilds-plugin`:
# another server outside this repository mounts that one, and a dev deploy must never
# replace the library it runs.
DRAGONWILDS_PLUGIN_DIR="${DRAGONWILDS_PLUGIN_DIR:-${DS_DATA}/dragonwilds-dev-plugin}"
DRAGONWILDS_STATE_DIR="${DRAGONWILDS_STATE_DIR:-${DS_DATA}/dragonwilds-dev-state}"

install_dragonwilds() {
    # The dev rig uses its own project name, container and data dir
    # (takaro-dev-dragonwilds / _data/dragonwilds-dev).
    mkdir -p "${DS_DATA}/dragonwilds-dev" "${DRAGONWILDS_PLUGIN_DIR}" "${DRAGONWILDS_STATE_DIR}"

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

    # Build and load the native connector once the game is installed.
    "${DS_DIR}/scripts/deploy-connector.sh" dragonwilds
}

DRAGONWILDS_SRC="${DRAGONWILDS_SRC:-${REPO_ROOT}/games/dragonwilds}"
DRAGONWILDS_RIG_LOCK="${DRAGONWILDS_RIG_LOCK:-${DS_DATA}/dragonwilds-rig.lock}"
DRAGONWILDS_LEGACY_SIDECAR="takaro-dev-dragonwilds-sidecar"

deploy_dragonwilds() {
    local plugin="${DRAGONWILDS_SRC}/mod" dest="${DRAGONWILDS_PLUGIN_DIR}"
    local build="${plugin}/build.sh"

    if [ ! -f "$build" ]; then
        ds_warn "Dragonwilds plugin source not present yet (${build}) — nothing to deploy."
        return 0
    fi

    # The .so is built in the plugin's debian:bookworm toolchain container, so the glibc it
    # links against is never newer than the one in the server image. build.sh runs its own
    # container and mounts its own source tree, so call it directly.
    ds_info "Building libtakaro-dragonwilds.so (debian:bookworm toolchain)..."
    ( cd "$plugin" && bash ./build.sh ) || ds_die "Dragonwilds plugin build failed"
    local so="${plugin}/dist/libtakaro-dragonwilds.so"
    [ -f "$so" ] || ds_die "expected ${so} after build.sh"

    # An LD_PRELOAD object with an unresolvable symbol makes ld.so fail the WHOLE process:
    # the game container then crash-loops in `Restarting (127)`. Refuse to ship one. The
    # check runs in a toolchain container, because the host does not necessarily have binutils.
    ds_info "Checking the .so for undefined symbols before it is ever preloaded..."
    local undef
    # shellcheck disable=SC2016  # the awk program expands inside the container, not here
    undef="$(ds_toolchain_run debian:bookworm "$(dirname "$so")" bash -c '
        command -v nm >/dev/null 2>&1 || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq binutils >/dev/null 2>&1; }
        nm -D --undefined-only libtakaro-dragonwilds.so 2>/dev/null | awk "\$1 == \"U\" { print \$2 }" | grep -vE "GLIBC|GCC|CXXABI" || true
    ' 2>/dev/null || true)"
    if [ -n "$undef" ]; then
        printf '%s\n' "$undef" >&2
        ds_die "libtakaro-dragonwilds.so has undefined symbols outside the C/C++ runtime (see above) — refusing to deploy; LD_PRELOAD would crash-loop the server"
    fi
    ds_ok "no undefined symbols outside the C/C++ runtime"

    # The native connector and a 0.2.x sidecar must never hold the same Takaro identity at
    # once. Drain and remove the old sidecar first (games/dragonwilds/scripts/drain-legacy.py).
    if docker container inspect "$DRAGONWILDS_LEGACY_SIDECAR" >/dev/null 2>&1; then
        ds_die "legacy Dragonwilds sidecar container still exists; drain and remove it before native deployment"
    fi

    # The running game holds the .so open via LD_PRELOAD, so it cannot be replaced in place:
    # stop → swap → start, the whole sequence under the rig lock as one command.
    mkdir -p "$dest" "${DRAGONWILDS_STATE_DIR}" "$(dirname "$DRAGONWILDS_RIG_LOCK")"
    cp "$so" "${dest}/libtakaro-dragonwilds.so.new"

    local restart=0
    ds_is_running dragonwilds && restart=1
    [ "$restart" = 1 ] && ds_info "Swapping the plugin under the rig lock (stop → swap → start)..."
    flock -w 900 "$DRAGONWILDS_RIG_LOCK" -c "
        set -e
        if docker container inspect '${DRAGONWILDS_LEGACY_SIDECAR}' >/dev/null 2>&1; then
            echo 'legacy Dragonwilds sidecar container still exists; refusing native swap' >&2
            exit 1
        fi
        if [ '${restart}' = 1 ]; then '${DS_DIR}/scripts/stop.sh' dragonwilds; fi
        mv -f '${dest}/libtakaro-dragonwilds.so.new' '${dest}/libtakaro-dragonwilds.so'
        chmod 644 '${dest}/libtakaro-dragonwilds.so'
        if [ '${restart}' = 1 ]; then '${DS_DIR}/scripts/start.sh' dragonwilds; fi
    " || { rm -f "${dest}/libtakaro-dragonwilds.so.new"; ds_die "plugin swap under the rig lock failed"; }
    ds_ok "${dest}/libtakaro-dragonwilds.so"
}

ds_source_paths_dragonwilds() { echo "games/dragonwilds/mod games/dragonwilds/version.txt"; }
