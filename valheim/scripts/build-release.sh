#!/usr/bin/env bash
# Builds the Valheim connector at <version> and packages the BepInEx plugin DLLs
# into <out-dir>/takaro-valheim-plugin.zip. Requires Valheim, BepInEx, and Jotunn
# reference assemblies; run ./scripts/setup-environment.sh first for local defaults.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: build-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir>}"

DATA_DIR="${VALHEIM_DATA_DIR:-_data}"
VALHEIM_REFERENCE_PATH="${VALHEIM_REFERENCE_PATH:-${DATA_DIR}/server/valheim_server_Data/Managed}"
BEPINEX_REFERENCE_PATH="${BEPINEX_REFERENCE_PATH:-${DATA_DIR}/deps/bepinex/BepInExPack_Valheim/BepInEx/core}"
JOTUNN_REFERENCE_PATH="${JOTUNN_REFERENCE_PATH:-${DATA_DIR}/deps/jotunn/plugins}"

for path in "$VALHEIM_REFERENCE_PATH" "$BEPINEX_REFERENCE_PATH" "$JOTUNN_REFERENCE_PATH"; do
  [ -d "$path" ] || {
    echo "Missing reference path: $path" >&2
    echo "Run valheim/scripts/setup-environment.sh or set VALHEIM_REFERENCE_PATH, BEPINEX_REFERENCE_PATH, and JOTUNN_REFERENCE_PATH." >&2
    exit 1
  }
done

VALHEIM_REFERENCE_PATH="$(realpath "$VALHEIM_REFERENCE_PATH")"
BEPINEX_REFERENCE_PATH="$(realpath "$BEPINEX_REFERENCE_PATH")"
JOTUNN_REFERENCE_PATH="$(realpath "$JOTUNN_REFERENCE_PATH")"

mkdir -p "$OUT_DIR"
echo "Building Valheim connector v${VERSION}..."

dotnet restore Takaro.Valheim.sln
dotnet test Takaro.Valheim.sln --no-restore -v minimal
dotnet build src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -c Release \
  -f net472 \
  -p:EnableValheimPluginBuild=true \
  -p:BepInExReferencePath="$BEPINEX_REFERENCE_PATH" \
  -p:JotunnReferencePath="$JOTUNN_REFERENCE_PATH" \
  -p:ValheimReferencePath="$VALHEIM_REFERENCE_PATH"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

PLUGIN_DIR="$STAGE/TakaroValheim"
mkdir -p "$PLUGIN_DIR"
cp src/Takaro.Valheim.Plugin/bin/Release/net472/TakaroValheim.dll "$PLUGIN_DIR/"
cp src/Takaro.Valheim.Plugin/bin/Release/net472/Takaro.Valheim.Core.dll "$PLUGIN_DIR/"

cat > "$PLUGIN_DIR/README.txt" << EOF
Takaro Valheim Connector ${VERSION}

Install:
1. Install BepInExPack Valheim and Jotunn on the Valheim dedicated server.
2. Copy this folder into BepInEx/plugins/TakaroValheim.
3. Start the server once, then configure BepInEx/config/com.takaro.valheim.cfg.
4. Set registrationToken to the token from your Takaro game server connector setup.

Do not commit live registration tokens.
EOF

(cd "$STAGE" && zip -qr takaro-valheim-plugin.zip TakaroValheim)
cp "$STAGE/takaro-valheim-plugin.zip" "$OUT_DIR/"

echo "  -> $OUT_DIR/takaro-valheim-plugin.zip"
