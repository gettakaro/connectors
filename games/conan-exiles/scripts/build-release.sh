#!/usr/bin/env bash
# Builds the Conan Exiles TypeScript sidecar and packages a runtime zip into
# <out-dir>/takaro-conan-exiles-bridge.zip.
set -euo pipefail
cd "$(dirname "$0")/../bridge"

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
cp -R dist scripts package.json package-lock.json "$PACKAGE_DIR/"
cp ../README.md ../TakaroConfig.example.txt "$PACKAGE_DIR/"
rm -rf "$PACKAGE_DIR/dist/__tests__"

# The release must be runnable with `npm ci --omit=dev`, so every entrypoint a
# package.json script points at has to exist in the packaged dist/.
for required in dist/index.js dist/mod/pollerCli.js; do
  if [ ! -f "$PACKAGE_DIR/$required" ]; then
    echo "build-release: missing $required in release package" >&2
    exit 1
  fi
done

cat > "$PACKAGE_DIR/README.release.txt" << EOF
Takaro Conan Exiles Connector ${VERSION}

Install:
1. Extract this folder on the Conan Exiles dedicated server host.
2. Run npm ci --omit=dev.
3. Copy TakaroConfig.example.txt to TakaroConfig.txt.
4. Configure Takaro registration and Conan RCON values.
5. Start with npm start.
6. For in-game chat, start the helper as a second process: npm run mod-helper
   (see README.md for TAKARO_CONAN_CHAT_MOD / TAKARO_CONAN_RENDER_COMMAND).

Both npm start and npm run mod-helper run from dist/ and need only production
dependencies.

Do not commit live registration tokens or RCON passwords.
EOF

(cd "$STAGE" && zip -qr takaro-conan-exiles-bridge.zip TakaroConanExiles)
cp "$STAGE/takaro-conan-exiles-bridge.zip" "$OUT_DIR/"

echo "  -> $OUT_DIR/takaro-conan-exiles-bridge.zip"
