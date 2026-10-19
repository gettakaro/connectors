#!/usr/bin/env bash
# Validates every compose file with `docker compose config`, then checks the
# port map for collisions between games.
#
# Usage: validate.sh
#
# Runs without a real .env: required variables are given obvious placeholder
# values so the FILES are validated, not your credentials.
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../lib" && pwd)/common.sh"

ENV_FILE="${DS_ENV_FILE}"
[ -f "$ENV_FILE" ] || ENV_FILE="${DS_DIR}/.env.example"

# Placeholders only — validate.sh never needs real credentials.
export TAKARO_REGISTRATION_TOKEN="${TAKARO_REGISTRATION_TOKEN:-validate-placeholder}"
export RCON_PASSWORD="${RCON_PASSWORD:-validate-placeholder}"
export ADMIN_PASSWORD="${ADMIN_PASSWORD:-validate-placeholder}"
export VALHEIM_SERVER_PASSWORD="${VALHEIM_SERVER_PASSWORD:-validate-placeholder}"
export DRAGONWILDS_DEV_ADMIN_PASSWORD="${DRAGONWILDS_DEV_ADMIN_PASSWORD:-validate-placeholder}"
export DRAGONWILDS_DEV_WORLD_PASSWORD="${DRAGONWILDS_DEV_WORLD_PASSWORD:-validate-placeholder}"
export TAKARO_DRAGONWILDS_PLUGIN_TOKEN="${TAKARO_DRAGONWILDS_PLUGIN_TOKEN:-validate-placeholder}"
export TAKARO_ENSHROUDED_PLUGIN_TOKEN="${TAKARO_ENSHROUDED_PLUGIN_TOKEN:-validate-placeholder}"
export TAKARO_VEIN_PLUGIN_TOKEN="${TAKARO_VEIN_PLUGIN_TOKEN:-validate-placeholder}"
export VEIN_DEV_WORLD_PASSWORD="${VEIN_DEV_WORLD_PASSWORD:-validate-placeholder}"
export EXTERNAL_ADDRESS="${EXTERNAL_ADDRESS:-127.0.0.1}"
export DUNE_PG_PASSWORD="${DUNE_PG_PASSWORD:-validate-placeholder}"
export DUNE_GM_AUTH_TOKEN="${DUNE_GM_AUTH_TOKEN:-validate-placeholder}"
export DUNE_GM_AMQP_PASS="${DUNE_GM_AMQP_PASS:-validate-placeholder}"
export DUNE_RMQ_HTTP_TOKEN_AUTH_SECRET="${DUNE_RMQ_HTTP_TOKEN_AUTH_SECRET:-validate-placeholder}"
export DUNE_WORLD_PASSWORD="${DUNE_WORLD_PASSWORD:-validate-placeholder}"
export DUNE_WORLD_UNIQUE_NAME="${DUNE_WORLD_UNIQUE_NAME:-validate-placeholder}"
export TAKARO_DUNE_PLUGIN_TOKEN="${TAKARO_DUNE_PLUGIN_TOKEN:-validate-placeholder}"
export DUNE_SERVER_IMAGE="${DUNE_SERVER_IMAGE:-target-not-resolved:validate}"
export DUNE_GATEWAY_IMAGE="${DUNE_GATEWAY_IMAGE:-target-not-resolved:validate}"
export DUNE_DIRECTOR_IMAGE="${DUNE_DIRECTOR_IMAGE:-target-not-resolved:validate}"
export DUNE_TEXT_ROUTER_IMAGE="${DUNE_TEXT_ROUTER_IMAGE:-target-not-resolved:validate}"
export DUNE_RABBITMQ_IMAGE="${DUNE_RABBITMQ_IMAGE:-target-not-resolved:validate}"
export DUNE_DB_UTILS_IMAGE="${DUNE_DB_UTILS_IMAGE:-target-not-resolved:validate}"
export DUNE_POSTGRES_IMAGE="${DUNE_POSTGRES_IMAGE:-target-not-resolved:validate}"
export DUNE_DB_NAME="${DUNE_DB_NAME:-dune_target_not_resolved}"
export DUNE_IMAGE_TAG="${DUNE_IMAGE_TAG:-validate}"

ds_info "Validating compose files against ${ENV_FILE##*/}"
rc=0
ports_tmp="$(mktemp)"
# This script's own EXIT trap replaces common.sh's, so it runs that cleanup too --
# otherwise a scratch directory registered while validating would survive the run.
trap 'rm -f "$ports_tmp"; ds_cleanup_scratch' EXIT

for file in "${DS_COMPOSE_DIR}"/*.yml; do
    name="$(basename "$file")"
    args=(--env-file "$ENV_FILE" -f "$file")
    # Profiled services are hidden from `config` unless their profile is on.
    if grep -q '^ *profiles:' "$file"; then
        while read -r p; do args+=(--profile "$p"); done < <(
            grep -oE '^ *profiles: \["[a-z-]+"\]' "$file" | grep -oE '"[a-z-]+"' | tr -d '"'
        )
    fi

    if out="$( cd "$DS_COMPOSE_DIR" && docker compose "${args[@]}" config 2>&1 )"; then
        ds_ok "$name"
        # Collect published host ports for the collision check.
        printf '%s' "$out" | python3 -c '
import sys, re
doc = sys.stdin.read()
for m in re.finditer(r"published:\s*\"?(\d+)\"?\s*\n\s*protocol:\s*(\w+)", doc):
    print(f"{m.group(1)}/{m.group(2)}")
' >> "$ports_tmp" || true
    else
        printf '\033[31m  FAIL\033[0m %s\n%s\n' "$name" "$out"
        rc=1
    fi
done

echo
ds_info "Checking for host port collisions between games"
if dupes="$(sort "$ports_tmp" | uniq -d)" && [ -n "$dupes" ]; then
    printf '\033[31m  collision on:\033[0m %s\n' "$(printf '%s' "$dupes" | tr '\n' ' ')"
    rc=1
else
    ds_ok "no collisions across $(sort -u "$ports_tmp" | wc -l) published ports"
fi

exit "$rc"
