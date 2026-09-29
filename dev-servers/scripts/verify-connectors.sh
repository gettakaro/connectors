#!/usr/bin/env bash
# Boots each game in turn, waits for proof that its connector reached Takaro,
# then stops it again. One game at a time, so the RAM budget is never at risk.
#
# Usage: verify-connectors.sh [game...] [--keep] [--timeout N]
#   --keep       leave each game running after a successful check
#   --timeout N  seconds to wait per game (default 420)
set -euo pipefail
DS_LIB="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../lib" && pwd)"
# shellcheck source=../lib/common.sh
. "${DS_LIB}/common.sh"

KEEP=0
TIMEOUT=420
GAMES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --keep)    KEEP=1 ;;
        --timeout) shift; TIMEOUT="$1" ;;
        -*)        ds_die "unknown option $1" ;;
        *)         ds_validate_game "$1"; GAMES+=("$1") ;;
    esac
    shift
done
[ ${#GAMES[@]} -gt 0 ] || mapfile -t GAMES < <(ds_game_ids)

ds_load_env
ds_require_token

# Log line proving the connector reached Takaro; each game defines its own in
# lib/games/<game>.sh. A game that defines none has no Takaro connector to prove.
ds_success_pattern() { ds_dispatch_or __NO_CONNECTOR__ ds_success_pattern "$1"; }

# Where that line appears. Container output by default; a game whose connector writes its
# own log file defines ds_connector_logs_<game> in lib/games/<game>.sh.
ds_connector_logs() {
    local fn
    fn="ds_connector_logs_$(ds_fn_id "$1")"
    if declare -F "$fn" >/dev/null; then "$fn"; else ds_compose "$1" logs --tail 2000 2>/dev/null; fi
}

ds_failure_pattern() {
    # A socket that opens and immediately closes is a failure, not a pass.
    echo "Identify failed|identify failed|Invalid registrationToken|Takaro identify failed|reconnect disabled|WebSocket connection closed: 1006|Cannot send message - WebSocket not connected"
}

declare -A RESULT
for game in "${GAMES[@]}"; do
    if ! ds_is_installed "$game"; then
        RESULT[$game]="not installed"; continue
    fi

    pattern="$(ds_success_pattern "$game")"
    if [ "$pattern" = "__NO_CONNECTOR__" ]; then
        RESULT[$game]="n/a — no Takaro connector exists for this game"
        continue
    fi

    printf '\n\033[1m──────── %s ────────\033[0m\n' "$game"
    already_running=0
    ds_is_running "$game" && already_running=1

    if [ "$already_running" -eq 0 ]; then
        ds_info "Starting ${game}..."
        "${DS_DIR}/scripts/start.sh" "$game" >/dev/null 2>&1 || {
            RESULT[$game]="FAILED to start"; continue
        }
    fi

    ds_info "Waiting up to ${TIMEOUT}s for Takaro contact..."
    waited=0; outcome="TIMEOUT — no Takaro contact"
    while [ "$waited" -lt "$TIMEOUT" ]; do
        logs="$(ds_connector_logs "$game" || true)"
        if grep -qE "$(ds_failure_pattern)" <<< "$logs"; then
            outcome="REJECTED — $(grep -oE "$(ds_failure_pattern)[^\"]{0,60}" <<< "$logs" | tail -1)"
            break
        fi
        if grep -qE "$pattern" <<< "$logs"; then
            outcome="OK"
            break
        fi
        sleep 10
        waited=$((waited + 10))
    done

    case "$outcome" in
        OK) ds_ok "${game}: connector reached Takaro" ;;
        *)  ds_warn "${game}: ${outcome}" ;;
    esac
    RESULT[$game]="$outcome"

    if [ "$KEEP" -eq 0 ] && [ "$already_running" -eq 0 ]; then
        "${DS_DIR}/scripts/stop.sh" "$game" >/dev/null 2>&1 || true
    fi
done

echo
printf '\033[1m════════ connector verification ════════\033[0m\n'
for game in "${GAMES[@]}"; do
    r="${RESULT[$game]:-skipped}"
    case "$r" in
        OK)   printf '  \033[32m%-20s PASS\033[0m\n' "$game" ;;
        n/a*) printf '  %-20s %s\n' "$game" "$r" ;;
        *)    printf '  \033[33m%-20s %s\033[0m\n' "$game" "$r" ;;
    esac
done
