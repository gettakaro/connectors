#!/usr/bin/env bash
# Builds the Terraria TypeScript sidecar and packages a runtime zip into
# <out-dir>/takaro-terraria-bridge.zip.
set -euo pipefail
cd "$(dirname "$0")/../bridge"

VERSION="${1:?usage: build-bridge-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-bridge-release.sh <version> <out-dir>}"

mkdir -p "$OUT_DIR"
echo "Building Terraria bridge v${VERSION}..."

npm ci
npm test
npm run build

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

PACKAGE_DIR="$STAGE/TakaroTerrariaBridge"
mkdir -p "$PACKAGE_DIR"
cp -R dist TakaroConfig.example.txt package.json package-lock.json "$PACKAGE_DIR/"
cp ../README.md "$PACKAGE_DIR/"
rm -rf "$PACKAGE_DIR/dist/__tests__"

cat > "$PACKAGE_DIR/README.release.txt" << RELEOF
Takaro Terraria Bridge ${VERSION}

Install:
1. Extract this folder on the TShock server host.
2. Run npm ci --omit=dev.
3. Copy TakaroConfig.example.txt to TakaroConfig.txt.
4. Enable RestApiEnabled in tshock/config.json and set a REST token.
5. Configure Takaro registration and TShock REST values.
6. Start with npm start.

Do not commit live registration tokens or TShock REST tokens.
RELEOF

(cd "$STAGE" && zip -qr takaro-terraria-bridge.zip TakaroTerrariaBridge)
cp "$STAGE/takaro-terraria-bridge.zip" "$OUT_DIR/"

echo "  -> $OUT_DIR/takaro-terraria-bridge.zip"
