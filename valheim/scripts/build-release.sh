#!/usr/bin/env bash
# Builds the Valheim connector at <version> and packages the BepInEx plugin DLLs
# into <out-dir>/takaro-valheim-plugin.zip. Requires Valheim and BepInEx
# reference assemblies; run ./scripts/setup-environment.sh first for local defaults.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: build-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir>}"
SEMVER_PATTERN='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$'

is_valid_semver() {
  local candidate="$1"
  local release_and_prerelease
  local prerelease
  local identifier
  local -a identifiers

  [[ "$candidate" =~ $SEMVER_PATTERN ]] || return 1
  release_and_prerelease="${candidate%%+*}"
  if [[ "$release_and_prerelease" == *-* ]]; then
    prerelease="${release_and_prerelease#*-}"
    IFS='.' read -r -a identifiers <<< "$prerelease"
    for identifier in "${identifiers[@]}"; do
      if [[ "$identifier" =~ ^[0-9]+$ && ${#identifier} -gt 1 && "$identifier" == 0* ]]; then
        return 1
      fi
    done
  fi
}

if ! is_valid_semver "$VERSION"; then
  echo "Invalid semantic version: $VERSION" >&2
  echo "Expected SemVer such as 1.2.3, 1.2.3-rc.1, or 1.2.3+build.4." >&2
  exit 2
fi
CORE_VERSION="${VERSION%%[-+]*}"
ASSEMBLY_VERSION="${CORE_VERSION}.0"

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
echo "Building Valheim connector v${VERSION}..."

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
  -p:TakaroValheimPluginVersion="$VERSION" \
  -p:Version="$VERSION" \
  -p:PackageVersion="$VERSION" \
  -p:AssemblyVersion="$ASSEMBLY_VERSION" \
  -p:FileVersion="$ASSEMBLY_VERSION" \
  -p:InformationalVersion="$VERSION" \
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
Takaro Valheim Connector ${VERSION}

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
  "version": "${VERSION}",
  "architecture": "dedicated-server-only-bepinex-plugin"
}
EOF

(cd "$STAGE" && zip -qr takaro-valheim-plugin.zip TakaroValheim)
cp "$STAGE/takaro-valheim-plugin.zip" "$OUT_DIR/"

echo "  -> $OUT_DIR/takaro-valheim-plugin.zip"
