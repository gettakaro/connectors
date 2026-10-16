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

ds_prepare_data_dir "$GAME"
mkdir -p "$(dirname "$(ds_marker "$GAME")")"

# Every server gets its own identity token so one Takaro organisation can hold
# all of them at once. Exported for template rendering.
export TAKARO_WS_URL="${TAKARO_WS_URL:-wss://connect.takaro.io/}"

cleanup_stop() {
    ds_info "Stopping ${GAME} (install leaves games stopped)"
    ds_compose "$GAME" stop >/dev/null 2>&1 || true
}

# ── Run ──────────────────────────────────────────────────────────────────────

ds_info "Installing ${GAME} — $(ds_description "$GAME")"
ds_info "Estimated download/disk: ~$(ds_disk_gb "$GAME") GB; free now: $(ds_free_disk_gb "$GAME") GB"

ds_dispatch install "$GAME"

cleanup_stop
date -Iseconds > "$(ds_marker "$GAME")"
ds_ok "${GAME} installed and stopped. Start it with: dev-servers/scripts/start.sh ${GAME}"
