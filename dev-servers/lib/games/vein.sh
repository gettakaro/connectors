#!/usr/bin/env bash
# VEIN — one native LD_PRELOAD connector in the game process.
#
# Sourced by dev-servers/lib/common.sh. It registers the game in the shared registry and
# defines the steps install.sh, deploy-connector.sh and verify-connectors.sh dispatch to.

ds_register 75 'vein|vein.yml|-|vein|4|20|plugin|VEIN (Linux, app 2131400) + native Takaro LD_PRELOAD connector|vein-dev'

install_vein() {
    # VEIN dev rig (Steam app 2131400, anonymous login). Version-locked after the
    # install: a client must match the server build id, so the rig never updates
    # itself behind our back (VEIN_AUTO_UPDATE=false in compose/vein.yml).
    mkdir -p "${DS_DATA}/vein-dev" "${DS_DATA}/vein-plugin" "${DS_DATA}/vein-sidecar"

    ds_info "Building the VEIN server image (steamcmd base + Takaro entrypoint)..."
    ds_compose vein build vein

    ds_info "First boot: downloading VEIN server files (SteamCMD app 2131400)..."
    ds_info "The first SteamCMD attempt sometimes fails; the entrypoint retries."
    ds_compose vein up -d vein

    local bin="${DS_DATA}/vein-dev/Vein/Binaries/Linux/VeinServer-Linux-Test"
    ds_wait_for_condition vein "[ -s '${bin}' ]" 5400 "VEIN server binary" \
        || ds_die "VEIN server files never appeared; check 'dev-servers/scripts/logs.sh vein'"
    ds_wait_for_condition vein \
        "ds_steam_app_ready '${DS_DATA}/vein-dev/steamapps/appmanifest_2131400.acf'" \
        1800 "VEIN install manifest (fully installed)" \
        || ds_warn "appmanifest never reached StateFlags 4 — the server may re-download on the next boot"

    # First boot must reach readiness before we call this installed. This build never
    # prints "Created session GameSession." (that marker comes from an older build);
    # the real signals are the Steam heartbeat line in Vein.log and the built-in HTTP
    # API answering on 127.0.0.1:8080.
    ds_wait_for_condition vein \
        "grep -aq 'LogRamjetNetworking: Heartbeating' '${DS_DATA}/vein-dev/Vein/Saved/Logs/Vein.log' 2>/dev/null" \
        1800 "VEIN first boot (LogRamjetNetworking: Heartbeating)" \
        || ds_die "VEIN never reached its heartbeat; check 'dev-servers/scripts/logs.sh vein'"
    ds_wait_for_condition vein \
        "docker exec takaro-dev-vein curl -fsS -m 5 http://127.0.0.1:8080/status >/dev/null 2>&1" \
        300 "VEIN built-in HTTP API (127.0.0.1:8080/status)" \
        || ds_warn "the built-in HTTP API did not answer within 5 min"

    ds_fix_ownership "${DS_DATA}/vein-dev"

    # Build and load the native connector once the game is installed.
    "${DS_DIR}/scripts/deploy-connector.sh" vein
}

VEIN_SRC="${VEIN_SRC:-${REPO_ROOT}/games/vein}"
VEIN_RIG_LOCK="${VEIN_RIG_LOCK:-${DS_DATA}/vein-rig.lock}"

deploy_vein() {
    local src="$VEIN_SRC" plugin dest="${DS_DATA}/vein-plugin"
    plugin="${src}/mod"
    [ -d "$plugin" ] || plugin="${src}/plugin"   # source mirror uses plugin/, this repo uses mod/
    local build="${plugin}/build.sh"

    if [ ! -f "$build" ]; then
        ds_warn "VEIN plugin source not present yet (${build}) — nothing to deploy."
        return 0
    fi

    # The .so is built in a debian:bookworm toolchain container so the glibc it links
    # against is never newer than the one in the server image. build.sh runs its own
    # container and mounts its own source tree, so call it directly.
    ds_info "Building libtakaro-vein.so (debian:bookworm toolchain)..."
    ( cd "$plugin" && bash ./build.sh ) || ds_die "VEIN plugin build failed"
    local so="${plugin}/dist/libtakaro-vein.so"
    [ -f "$so" ] || ds_die "expected ${so} after build.sh"

    # An LD_PRELOAD object with an unresolvable symbol makes ld.so fail the WHOLE
    # process: the game container then crash-loops in `Restarting (127)`. A preload
    # object must have no undefined symbols beyond the C/C++ runtime, so refuse to
    # ship one that does. The check runs in the toolchain container, because the
    # host does not necessarily have binutils.
    ds_info "Checking the .so for undefined symbols before it is ever preloaded..."
    local undef
    undef="$(ds_toolchain_run debian:bookworm "$(dirname "$so")" bash -c '
        apt-get update -qq >/dev/null 2>&1 || true
        command -v nm >/dev/null 2>&1 || apt-get install -y -qq binutils >/dev/null 2>&1
        nm -D --undefined-only libtakaro-vein.so 2>/dev/null | grep -vE "GLIBC|GCC|CXXABI" || true
    ' 2>/dev/null || true)"
    if [ -n "$undef" ]; then
        printf '%s\n' "$undef" >&2
        ds_die "libtakaro-vein.so has undefined symbols outside the C/C++ runtime (see above) — refusing to deploy; LD_PRELOAD would crash-loop the server"
    fi
    ds_ok "no undefined symbols outside the C/C++ runtime"

    # The running game holds the .so open via LD_PRELOAD, so it cannot be replaced in
    # place: stop → swap → start, the whole sequence under the rig lock as one command.
    mkdir -p "$dest" "$(dirname "$VEIN_RIG_LOCK")"
    cp "$so" "${dest}/libtakaro-vein.so.new"

    # A legacy sidecar can reconnect using the same Takaro identity, even when
    # its Compose service has disappeared. Require the migration drain/removal
    # procedure before deploying the native-only service.
    if docker container inspect takaro-dev-vein-sidecar >/dev/null 2>&1; then
        rm -f "${dest}/libtakaro-vein.so.new"
        ds_die "legacy VEIN sidecar container still exists; drain and remove it before native deployment"
    fi

    if ds_is_running vein; then
        ds_info "Swapping the plugin under the rig lock (stop → swap → start)..."
        flock -w 900 "$VEIN_RIG_LOCK" -c "
            set -e
            if docker container inspect takaro-dev-vein-sidecar >/dev/null 2>&1; then
                echo 'legacy VEIN sidecar container still exists; refusing native swap' >&2
                exit 1
            fi
            '${DS_DIR}/scripts/stop.sh' vein
            mv -f '${dest}/libtakaro-vein.so.new' '${dest}/libtakaro-vein.so'
            chmod 644 '${dest}/libtakaro-vein.so'
            '${DS_DIR}/scripts/start.sh' vein
        " || ds_die "plugin swap under the rig lock failed"

    else
        flock -w 900 "$VEIN_RIG_LOCK" -c "
            set -e
            if docker container inspect takaro-dev-vein-sidecar >/dev/null 2>&1; then
                echo 'legacy VEIN sidecar container still exists; refusing native swap' >&2
                exit 1
            fi
            mv -f '${dest}/libtakaro-vein.so.new' '${dest}/libtakaro-vein.so'
            chmod 644 '${dest}/libtakaro-vein.so'
        " || ds_die "plugin swap under the rig lock failed"
    fi
    ds_ok "${dest}/libtakaro-vein.so"
}

ds_source_paths_vein() { echo "games/vein/mod games/vein/version.txt"; }
