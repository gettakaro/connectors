#!/usr/bin/env bash
# Downloads the Valheim dedicated server assemblies plus BepInEx reference
# DLLs needed to compile the Valheim connector plugin.
set -euo pipefail
cd "$(dirname "$0")/.."

DATA_DIR="${VALHEIM_DATA_DIR:-_data}"
STEAMCMD_DIR="${STEAMCMD_DIR:-${DATA_DIR}/steamcmd}"
SERVER_DIR="${VALHEIM_SERVER_DIR:-${DATA_DIR}/server}"
DEPS_DIR="${VALHEIM_DEPS_DIR:-${DATA_DIR}/deps}"
STEAMCMD="${STEAMCMD:-${STEAMCMD_DIR}/steamcmd.sh}"

BEPINEX_API="${BEPINEX_API:-https://thunderstore.io/api/experimental/package/denikson/BepInExPack_Valheim/}"
mkdir -p "$STEAMCMD_DIR" "$SERVER_DIR" "$DEPS_DIR"

if [ ! -x "$STEAMCMD" ]; then
  echo "Downloading SteamCMD..."
  curl -sL https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz \
    | tar -xzf - -C "$STEAMCMD_DIR"
fi

echo "Downloading/updating Valheim dedicated server..."
for attempt in 1 2 3; do
  if "$STEAMCMD" \
    +@sSteamCmdForcePlatformType linux \
    +force_install_dir "$(pwd)/$SERVER_DIR" \
    +login anonymous \
    +app_update 896660 validate \
    +quit; then
    break
  fi

  if [ "$attempt" -eq 3 ]; then
    echo "SteamCMD failed to install/update Valheim dedicated server after $attempt attempts." >&2
    exit 1
  fi

  echo "SteamCMD failed to install/update Valheim dedicated server; retrying ($attempt/3)..." >&2
  sleep $((attempt * 10))
done

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

echo "Reference assemblies ready:"
echo "  Valheim: $SERVER_DIR/valheim_server_Data/Managed"
echo "  BepInEx: $DEPS_DIR/bepinex/BepInExPack_Valheim/BepInEx/core"
