#!/usr/bin/env bash
# Builds the Conan Exiles TypeScript sidecar and packages a runtime zip into
# <out-dir>/takaro-conan-exiles-bridge.zip.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: build-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir>}"

mkdir -p "$OUT_DIR"
echo "Building Conan Exiles connector v${VERSION}..."

npm ci
npm test
npm run build

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

PACKAGE_DIR="$STAGE/TakaroConanExiles"
mkdir -p "$PACKAGE_DIR"
cp -R dist scripts README.md TakaroConfig.example.txt package.json package-lock.json "$PACKAGE_DIR/"
rm -rf "$PACKAGE_DIR/dist/__tests__"

cat > "$PACKAGE_DIR/README.release.txt" << EOF
Takaro Conan Exiles Connector ${VERSION}

Install:
1. Extract this folder on the Conan Exiles dedicated server host.
2. Run npm ci --omit=dev.
3. Copy TakaroConfig.example.txt to TakaroConfig.txt.
4. Configure Takaro registration and Conan RCON values.
5. Start with npm start.

Do not commit live registration tokens or RCON passwords.
EOF

(cd "$STAGE" && zip -qr takaro-conan-exiles-bridge.zip TakaroConanExiles)
cp "$STAGE/takaro-conan-exiles-bridge.zip" "$OUT_DIR/"

echo "  -> $OUT_DIR/takaro-conan-exiles-bridge.zip"
