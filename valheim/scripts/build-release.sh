#!/usr/bin/env bash
# Builds the Valheim connector at <version> and packages the BepInEx plugin DLLs
# into <out-dir>/takaro-valheim-plugin.zip. Requires Valheim and BepInEx
# reference assemblies; run ./scripts/setup-environment.sh first for local defaults.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: build-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir>}"
source scripts/release-version.sh

if resolve_valheim_release_version "$VERSION"; then
  :
else
  resolution_status=$?
  if [ "$resolution_status" -eq 2 ]; then
    echo "Unsupported semantic version: $VERSION" >&2
    echo "Major, minor, and patch cannot exceed 65534 because BepInEx and .NET assembly metadata require bounded numeric components." >&2
  else
    echo "Invalid semantic version: $VERSION" >&2
    echo "Expected SemVer such as 1.2.3, 1.2.3-rc.1, or 1.2.3+build.4." >&2
  fi
  exit 2
fi

DATA_DIR="${VALHEIM_DATA_DIR:-_data}"
VALHEIM_REFERENCE_PATH="${VALHEIM_REFERENCE_PATH:-${DATA_DIR}/server/valheim_server_Data/Managed}"
BEPINEX_REFERENCE_PATH="${BEPINEX_REFERENCE_PATH:-${DATA_DIR}/deps/bepinex/BepInExPack_Valheim/BepInEx/core}"

for path in "$VALHEIM_REFERENCE_PATH" "$BEPINEX_REFERENCE_PATH"; do
  [ -d "$path" ] || {
    echo "Missing reference path: $path" >&2
    echo "Run valheim/scripts/setup-environment.sh or set VALHEIM_REFERENCE_PATH and BEPINEX_REFERENCE_PATH." >&2
    exit 1
  }
done

VALHEIM_REFERENCE_PATH="$(realpath "$VALHEIM_REFERENCE_PATH")"
BEPINEX_REFERENCE_PATH="$(realpath "$BEPINEX_REFERENCE_PATH")"

mkdir -p "$OUT_DIR"
echo "Building Valheim connector v${VALHEIM_RELEASE_VERSION}..."

dotnet restore Takaro.Valheim.sln
dotnet test Takaro.Valheim.sln --no-restore -v minimal
PUBLISH_DIR="$(mktemp -d)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$PUBLISH_DIR" "$STAGE"' EXIT

dotnet publish src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -c Release \
  -f net472 \
  --no-restore \
  -o "$PUBLISH_DIR" \
  -p:EnableValheimPluginBuild=true \
  -p:BepInExReferencePath="$BEPINEX_REFERENCE_PATH" \
  -p:ValheimReferencePath="$VALHEIM_REFERENCE_PATH" \
  -p:TakaroValheimReleaseVersion="$VALHEIM_RELEASE_VERSION" \
  -p:TakaroValheimBepInExVersion="$VALHEIM_BEPINEX_VERSION" \
  -p:Version="$VALHEIM_RELEASE_VERSION" \
  -p:PackageVersion="$VALHEIM_RELEASE_VERSION" \
  -p:AssemblyVersion="$VALHEIM_ASSEMBLY_VERSION" \
  -p:FileVersion="$VALHEIM_ASSEMBLY_VERSION" \
  -p:InformationalVersion="$VALHEIM_RELEASE_VERSION" \
  -p:IncludeSourceRevisionInInformationalVersion=false

PLUGIN_DIR="$STAGE/TakaroValheim"
mkdir -p "$PLUGIN_DIR"
cp "$PUBLISH_DIR"/*.dll "$PLUGIN_DIR/"

# The game server already provides Valheim, Unity, BepInEx, and Harmony
# assemblies. Do not bundle those host/reference DLLs, but keep NuGet runtime
# dependencies such as System.Text.Json that are required on .NET Framework 4.7.2.
rm -f \
  "$PLUGIN_DIR/0Harmony.dll" \
  "$PLUGIN_DIR/BepInEx.dll" \
  "$PLUGIN_DIR/assembly_valheim.dll" \
  "$PLUGIN_DIR/assembly_utils.dll" \
  "$PLUGIN_DIR/Splatform.dll" \
  "$PLUGIN_DIR/UnityEngine.dll" \
  "$PLUGIN_DIR/UnityEngine.CoreModule.dll"

cat > "$PLUGIN_DIR/README.txt" << EOF
Takaro Valheim Connector ${VALHEIM_RELEASE_VERSION}

Install:
1. Install BepInExPack Valheim on the dedicated server. Do not install this plugin on clients.
2. Copy this folder into BepInEx/plugins/TakaroValheim.
3. Start the server once, then configure BepInEx/config/com.takaro.valheim.cfg.
4. Set registrationToken to the token from your Takaro game server connector setup.
5. Restart the dedicated server so the connector loads the saved token and configuration.

Do not commit live registration tokens.
EOF

cat > "$PLUGIN_DIR/manifest.json" << EOF
{
  "name": "TakaroValheim",
  "version": "${VALHEIM_RELEASE_VERSION}",
  "architecture": "dedicated-server-only-bepinex-plugin"
}
EOF

(cd "$STAGE" && zip -qr takaro-valheim-plugin.zip TakaroValheim)
cp "$STAGE/takaro-valheim-plugin.zip" "$OUT_DIR/"

echo "  -> $OUT_DIR/takaro-valheim-plugin.zip"
