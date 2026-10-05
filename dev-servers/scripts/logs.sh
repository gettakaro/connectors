#!/usr/bin/env bash
# Tails a game's container logs.
#
# Usage: logs.sh <game> [docker compose logs args...]
#   logs.sh valheim              # last 100 lines
#   logs.sh valheim -f           # follow
#   logs.sh conan-exiles --tail 500 conan-exiles
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../lib" && pwd)/common.sh"

GAME="${1:?usage: logs.sh <game> [docker compose logs args...]}"
shift
ds_validate_game "$GAME"
ds_load_env

if [ $# -eq 0 ]; then
    read -r -a services <<< "$(ds_services "$GAME")"
    exec_args=(--tail 100 "${services[@]}")
else
    exec_args=("$@")
fi

ds_compose "$GAME" logs "${exec_args[@]}"
