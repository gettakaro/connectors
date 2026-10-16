#!/usr/bin/env bash
# Installs every game (or the ones named) STRICTLY ONE AT A TIME, so a large
# download or a booting server never competes with another for RAM or disk.
# Each game is stopped before the next one starts.
#
# Usage: install-all.sh [game...] [--force]
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../lib" && pwd)/common.sh"

FORCE=""
GAMES=()
for arg in "$@"; do
    case "$arg" in
        --force) FORCE="--force" ;;
        -*)      ds_die "unknown option ${arg}" ;;
        *)       ds_validate_game "$arg"; GAMES+=("$arg") ;;
    esac
done
[ ${#GAMES[@]} -gt 0 ] || mapfile -t GAMES < <(ds_game_ids)

ds_load_env
if [ -z "${TAKARO_REGISTRATION_TOKEN:-}" ]; then
    ds_warn "TAKARO_REGISTRATION_TOKEN is empty — downloading anyway."
    ds_warn "Set it in dev-servers/.env, then run: dev-servers/scripts/reconfigure.sh"
fi

MIN_FREE="${DEV_SERVERS_MIN_FREE_GB:-40}"
TOTAL_DISK=0
for g in "${GAMES[@]}"; do TOTAL_DISK=$((TOTAL_DISK + $(ds_disk_gb "$g"))); done

ds_info "Installing ${#GAMES[@]} game(s), one at a time: ${GAMES[*]}"
ds_info "Estimated total disk: ~${TOTAL_DISK} GB; free now: $(ds_free_disk_gb) GB"
echo

FAILED=()
INSTALLED=()
SKIPPED=()

for game in "${GAMES[@]}"; do
    free="$(ds_free_disk_gb "$game")"
    if [ "$free" -lt "$MIN_FREE" ]; then
        ds_die "only ${free} GB free, below DEV_SERVERS_MIN_FREE_GB=${MIN_FREE}. Stopping before ${game}."
    fi

    if ds_is_installed "$game" && [ -z "$FORCE" ]; then
        ds_info "Skipping ${game} (already installed)"
        SKIPPED+=("$game")
        continue
    fi

    printf '\n\033[1m──────── %s ────────\033[0m\n' "$game"
    if "${DS_DIR}/scripts/install.sh" "$game" ${FORCE:+--force}; then
        INSTALLED+=("$game")
    else
        ds_warn "${game} failed to install — continuing with the rest"
        FAILED+=("$game")
    fi

    # Belt-and-braces: never leave a game running between installs.
    ds_compose "$game" stop >/dev/null 2>&1 || true
done

echo
printf '\033[1m════════ summary ════════\033[0m\n'
[ ${#INSTALLED[@]} -gt 0 ] && printf '  installed: %s\n' "${INSTALLED[*]}"
[ ${#SKIPPED[@]}   -gt 0 ] && printf '  skipped:   %s\n' "${SKIPPED[*]}"
[ ${#FAILED[@]}    -gt 0 ] && printf '  \033[31mfailed:    %s\033[0m\n' "${FAILED[*]}"
printf '  free disk: %s GB\n' "$(ds_free_disk_gb)"
echo
ds_info "All games are stopped. Start one with: dev-servers/scripts/start.sh <game>"

[ ${#FAILED[@]} -eq 0 ]
