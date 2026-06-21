#!/usr/bin/env bash
# Builds and packages the Terraria TShock plugin into <out-dir>.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: build-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir>}"

mkdir -p "$OUT_DIR"
echo "Building Terraria plugin v${VERSION}..."

./scripts/build-mod.sh

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

PLUGIN_DIR="$STAGE/TakaroTerrariaEvents"
mkdir -p "$PLUGIN_DIR"
cp _data/build/TakaroTerrariaEvents/TakaroTerrariaEvents.dll "$PLUGIN_DIR/"

cat > "$PLUGIN_DIR/README.txt" << EOF
Takaro Terraria Events Plugin ${VERSION}

Install:
1. Install TShock on the Terraria dedicated server.
2. Copy TakaroTerrariaEvents.dll into the TShock server plugin directory.
3. Restart the server.
4. Confirm the TShock log contains "Takaro Terraria Events plugin loaded".
5. Grant the connector/admin user the "takaro.admin" TShock permission.

This is a server-side TShock plugin. It does not require a client mod.
EOF

(cd "$STAGE" && zip -qr takaro-terraria-plugin.zip TakaroTerrariaEvents)
cp "$STAGE/takaro-terraria-plugin.zip" "$OUT_DIR/"

echo "  -> $OUT_DIR/takaro-terraria-plugin.zip"
