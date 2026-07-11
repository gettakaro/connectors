#!/usr/bin/env bash
# Builds separate dedicated-server and graphical-client Valheim BepInEx archives.
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
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-315532800}"

for path in "$VALHEIM_REFERENCE_PATH" "$BEPINEX_REFERENCE_PATH"; do
  [ -d "$path" ] || {
    echo "Missing reference path: $path" >&2
    echo "Run valheim/scripts/setup-environment.sh or set VALHEIM_REFERENCE_PATH and BEPINEX_REFERENCE_PATH." >&2
    exit 1
  }
done
if ! [[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || [ "$SOURCE_DATE_EPOCH" -lt 315532800 ]; then
  echo "SOURCE_DATE_EPOCH must be an integer Unix timestamp at or after 1980-01-01." >&2
  exit 2
fi

VALHEIM_REFERENCE_PATH="$(realpath "$VALHEIM_REFERENCE_PATH")"
BEPINEX_REFERENCE_PATH="$(realpath "$BEPINEX_REFERENCE_PATH")"
mkdir -p "$OUT_DIR"
OUT_DIR="$(realpath "$OUT_DIR")"
echo "Building Valheim connector and companion v${VALHEIM_RELEASE_VERSION}..."

dotnet restore Takaro.Valheim.sln
dotnet test Takaro.Valheim.sln --no-restore -v minimal

SERVER_PUBLISH="$(mktemp -d)"
CLIENT_PUBLISH="$(mktemp -d)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$SERVER_PUBLISH" "$CLIENT_PUBLISH" "$STAGE"' EXIT

dotnet publish src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -c Release \
  -f net472 \
  --no-restore \
  -o "$SERVER_PUBLISH" \
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
  -p:IncludeSourceRevisionInInformationalVersion=false \
  -p:ContinuousIntegrationBuild=true \
  -p:Deterministic=true \
  -p:PathMap="$(pwd)=/src"

dotnet publish src/Takaro.Valheim.Companion/Takaro.Valheim.Companion.csproj \
  -c Release \
  -f net472 \
  --no-restore \
  -o "$CLIENT_PUBLISH" \
  -p:EnableValheimCompanionBuild=true \
  -p:BepInExReferencePath="$BEPINEX_REFERENCE_PATH" \
  -p:ValheimReferencePath="$VALHEIM_REFERENCE_PATH" \
  -p:TakaroValheimCompanionReleaseVersion="$VALHEIM_RELEASE_VERSION" \
  -p:TakaroValheimCompanionBepInExVersion="$VALHEIM_BEPINEX_VERSION" \
  -p:Version="$VALHEIM_RELEASE_VERSION" \
  -p:PackageVersion="$VALHEIM_RELEASE_VERSION" \
  -p:AssemblyVersion="$VALHEIM_ASSEMBLY_VERSION" \
  -p:FileVersion="$VALHEIM_ASSEMBLY_VERSION" \
  -p:InformationalVersion="$VALHEIM_RELEASE_VERSION" \
  -p:IncludeSourceRevisionInInformationalVersion=false \
  -p:ContinuousIntegrationBuild=true \
  -p:Deterministic=true \
  -p:PathMap="$(pwd)=/src"

SERVER_DIR="$STAGE/TakaroValheim"
CLIENT_DIR="$STAGE/TakaroValheimCompanion"
mkdir -p "$SERVER_DIR" "$CLIENT_DIR"
cp "$SERVER_PUBLISH"/*.dll "$SERVER_DIR/"
cp "$CLIENT_PUBLISH"/*.dll "$CLIENT_DIR/"

strip_host_assemblies() {
  local package_dir="$1"
  rm -f \
    "$package_dir/0Harmony.dll" \
    "$package_dir/BepInEx.dll" \
    "$package_dir/assembly_valheim.dll" \
    "$package_dir/assembly_utils.dll" \
    "$package_dir/Splatform.dll" \
    "$package_dir/UnityEngine.dll" \
    "$package_dir"/UnityEngine.*.dll \
    "$package_dir/Jotunn.dll" \
    "$package_dir/ServerSync.dll"
}
strip_host_assemblies "$SERVER_DIR"
strip_host_assemblies "$CLIENT_DIR"
rm -f "$SERVER_DIR/Takaro.Valheim.Companion.dll"
rm -f "$CLIENT_DIR/TakaroValheim.dll" "$CLIENT_DIR/Takaro.Valheim.Core.dll"

cat > "$SERVER_DIR/README.txt" << EOF
Takaro Valheim Connector ${VALHEIM_RELEASE_VERSION}

Dedicated server install:
1. Install BepInExPack Valheim on the dedicated server.
2. Copy TakaroValheim into BepInEx/plugins/TakaroValheim.
3. Start once, then configure BepInEx/config/com.takaro.valheim.cfg.
4. Set the server registrationToken and companionMode.
5. Restart the dedicated server so the saved configuration is loaded.

This server package is not a client mod. Never commit live registration tokens.
EOF

cat > "$CLIENT_DIR/README.txt" << EOF
Takaro Valheim Companion ${VALHEIM_RELEASE_VERSION}

Graphical client install:
1. Install BepInExPack Valheim in the graphical Valheim client.
2. Copy TakaroValheimCompanion into BepInEx/plugins/TakaroValheimCompanion.
3. Restart Valheim. No Takaro token or cloud credential belongs on the client.

This client package is not the dedicated-server connector.
EOF

cat > "$SERVER_DIR/manifest.json" << EOF
{
  "name": "TakaroValheim",
  "productVersion": "${VALHEIM_RELEASE_VERSION}",
  "bepInExVersion": "${VALHEIM_BEPINEX_VERSION}",
  "processRole": "dedicated-server",
  "protocol": { "minimum": 1, "current": 1, "maximum": 1 }
}
EOF

cat > "$CLIENT_DIR/manifest.json" << EOF
{
  "name": "TakaroValheimCompanion",
  "productVersion": "${VALHEIM_RELEASE_VERSION}",
  "bepInExVersion": "${VALHEIM_BEPINEX_VERSION}",
  "processRole": "graphical-client",
  "protocol": { "minimum": 1, "current": 1, "maximum": 1 }
}
EOF

for required in \
  "$SERVER_DIR/TakaroValheim.dll" \
  "$SERVER_DIR/Takaro.Valheim.Core.dll" \
  "$SERVER_DIR/Takaro.Valheim.Companion.Protocol.dll" \
  "$CLIENT_DIR/Takaro.Valheim.Companion.dll" \
  "$CLIENT_DIR/Takaro.Valheim.Companion.Protocol.dll"; do
  [ -f "$required" ] || {
    echo "Publish output is missing required DLL: $required" >&2
    exit 1
  }
done

normalize_and_zip() {
  local folder_name="$1"
  local archive_name="$2"
  find "$STAGE/$folder_name" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
  (
    cd "$STAGE"
    find "$folder_name" -type f -print \
      | LC_ALL=C sort \
      | zip -X -q "$archive_name" -@
  )
  cp "$STAGE/$archive_name" "$OUT_DIR/$archive_name"
}

normalize_and_zip TakaroValheim takaro-valheim-plugin.zip
normalize_and_zip TakaroValheimCompanion takaro-valheim-companion.zip
bash tests/release-package-behavior.sh "$VALHEIM_RELEASE_VERSION" "$OUT_DIR"

echo "  -> $OUT_DIR/takaro-valheim-plugin.zip"
echo "  -> $OUT_DIR/takaro-valheim-companion.zip"
