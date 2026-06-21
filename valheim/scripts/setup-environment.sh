#!/usr/bin/env bash
# Downloads the Valheim dedicated server assemblies plus BepInEx/Jotunn reference
# DLLs needed to compile the Valheim connector plugin.
set -euo pipefail
cd "$(dirname "$0")/.."

DATA_DIR="${VALHEIM_DATA_DIR:-_data}"
STEAMCMD_DIR="${STEAMCMD_DIR:-${DATA_DIR}/steamcmd}"
SERVER_DIR="${VALHEIM_SERVER_DIR:-${DATA_DIR}/server}"
DEPS_DIR="${VALHEIM_DEPS_DIR:-${DATA_DIR}/deps}"
STEAMCMD="${STEAMCMD:-${STEAMCMD_DIR}/steamcmd.sh}"

BEPINEX_API="${BEPINEX_API:-https://thunderstore.io/api/experimental/package/denikson/BepInExPack_Valheim/}"
JOTUNN_API="${JOTUNN_API:-https://thunderstore.io/api/experimental/package/ValheimModding/Jotunn/}"

mkdir -p "$STEAMCMD_DIR" "$SERVER_DIR" "$DEPS_DIR"

if [ ! -x "$STEAMCMD" ]; then
  echo "Downloading SteamCMD..."
  curl -sL https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz \
    | tar -xzf - -C "$STEAMCMD_DIR"
fi

echo "Downloading/updating Valheim dedicated server..."
"$STEAMCMD" \
  +@sSteamCmdForcePlatformType linux \
  +force_install_dir "$(pwd)/$SERVER_DIR" \
  +login anonymous \
  +app_update 896660 validate \
  +quit

download_thunderstore_package() {
  local api_url="$1"
  local out_zip="$2"
  local download_url
  download_url="$(curl -fsSL "$api_url" | jq -r '.latest.download_url')"
  if [ -z "$download_url" ] || [ "$download_url" = "null" ]; then
    echo "Could not resolve Thunderstore download URL from $api_url" >&2
    exit 1
  fi
  curl -fsSL "$download_url" -o "$out_zip"
}

echo "Downloading BepInExPack Valheim..."
rm -rf "$DEPS_DIR/bepinex"
mkdir -p "$DEPS_DIR/bepinex"
download_thunderstore_package "$BEPINEX_API" "$DEPS_DIR/bepinex.zip"
unzip -q "$DEPS_DIR/bepinex.zip" -d "$DEPS_DIR/bepinex"

echo "Downloading Jotunn..."
rm -rf "$DEPS_DIR/jotunn"
mkdir -p "$DEPS_DIR/jotunn"
download_thunderstore_package "$JOTUNN_API" "$DEPS_DIR/jotunn.zip"
unzip -q "$DEPS_DIR/jotunn.zip" -d "$DEPS_DIR/jotunn"

echo "Reference assemblies ready:"
echo "  Valheim: $SERVER_DIR/valheim_server_Data/Managed"
echo "  BepInEx: $DEPS_DIR/bepinex/BepInExPack_Valheim/BepInEx/core"
echo "  Jotunn:  $DEPS_DIR/jotunn/plugins"
