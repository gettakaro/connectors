#!/usr/bin/env bash
# Extracts the TShock/Terraria reference DLLs needed to compile the server-side
# Terraria plugin. No runtime secrets are required.
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE="${TSHOCK_IMAGE:-ghcr.io/pryaxis/tshock:stable}"
REFS_DIR="${TERRARIA_REFS_DIR:-_data/refs}"

mkdir -p "$REFS_DIR"

container="$(docker create "$IMAGE")"
cleanup() {
  docker rm -f "$container" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker cp "$container:/server/ServerPlugins/TShockAPI.dll" "$REFS_DIR/TShockAPI.dll"
docker cp "$container:/server/bin/TerrariaServer.dll" "$REFS_DIR/TerrariaServer.dll"
docker cp "$container:/server/bin/OTAPI.dll" "$REFS_DIR/OTAPI.dll"

echo "Reference assemblies ready in terraria/$REFS_DIR"
