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
install_valheim_server() {
  local platforms
  local platform
  local attempt
  local exit_code=0

  read -r -a platforms <<< "${VALHEIM_STEAM_PLATFORMS:-linux windows}"

  for platform in "${platforms[@]}"; do
    for attempt in 1 2 3; do
      if "$STEAMCMD" \
        +@sSteamCmdForcePlatformType "$platform" \
        +force_install_dir "$(pwd)/$SERVER_DIR" \
        +login anonymous \
        +app_update 896660 validate \
        +quit; then
        exit_code=0
      else
        exit_code=$?
      fi

      if [ -d "$SERVER_DIR/valheim_server_Data/Managed" ]; then
        echo "Valheim dedicated server references installed for Steam platform '$platform'."
        return 0
      fi

      if [ "$exit_code" -eq 0 ]; then
        exit_code=1
      fi

      if [ "$attempt" -lt 3 ]; then
        echo "SteamCMD Valheim install attempt $attempt for platform '$platform' failed with exit code $exit_code; retrying..."
        rm -rf "$HOME/Steam/appcache" "$STEAMCMD_DIR/appcache"
        sleep $((attempt * 10))
      fi
    done

    if [ "$platform" = "linux" ]; then
      echo "Linux Valheim server references were not available; trying Windows Steam depot for compile references..."
      rm -rf "$SERVER_DIR"
      mkdir -p "$SERVER_DIR"
    fi
  done

  if [ -d "$SERVER_DIR/valheim_server_Data/Managed" ]; then
    return 0
  fi

  return "$exit_code"
}

install_valheim_server

curl_retry() {
  curl --retry 5 --retry-delay 2 --retry-all-errors "$@"
}

download_thunderstore_package() {
  local api_url="$1"
  local out_zip="$2"
  local download_url
  download_url="$(curl_retry -fsSL "$api_url" | jq -r '.latest.download_url')"
  if [ -z "$download_url" ] || [ "$download_url" = "null" ]; then
    echo "Could not resolve Thunderstore download URL from $api_url" >&2
    exit 1
  fi
  curl_retry -fsSL "$download_url" -o "$out_zip"
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
