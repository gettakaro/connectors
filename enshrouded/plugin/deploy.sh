#!/usr/bin/env bash
# Deploy build/dbghelp.dll to the dev-servers Enshrouded container.
# The DLL is bind-mounted read-only from _data/enshrouded-plugin/dbghelp.dll (see compose/enshrouded.yml),
# so SteamCMD updates of /opt/enshrouded/server cannot overwrite it. The server holds the DLL open, so
# the container is stopped (graceful save) before the file is replaced in place (same inode).
set -euo pipefail
cd "$(dirname "$0")"
DEV=${DEV_SERVERS:-$(cd ../.. && pwd)/dev-servers}
COMPOSE=(docker compose --env-file "$DEV/.env" -f "$DEV/compose/enshrouded.yml")
"${COMPOSE[@]}" stop
cp build/dbghelp.dll "$DEV/_data/enshrouded-plugin/dbghelp.dll"
"${COMPOSE[@]}" start
docker exec takaro-dev-enshrouded md5sum /opt/enshrouded/server/dbghelp.dll
md5sum build/dbghelp.dll
