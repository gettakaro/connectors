#!/usr/bin/env bash
# Downloads the Valheim dedicated-server assemblies plus BepInEx reference DLLs
# needed to compile the Valheim connector plugin.
set -euo pipefail
cd "$(dirname "$0")/.."

DATA_DIR="${VALHEIM_DATA_DIR:-_data}"
STEAMCMD_DIR="${STEAMCMD_DIR:-${DATA_DIR}/steamcmd}"
SERVER_DIR="${VALHEIM_SERVER_DIR:-${DATA_DIR}/server}"
DEPS_DIR="${VALHEIM_DEPS_DIR:-${DATA_DIR}/deps}"
STEAMCMD="${STEAMCMD:-${STEAMCMD_DIR}/steamcmd.sh}"
VALHEIM_STEAM_PLATFORMS="${VALHEIM_STEAM_PLATFORMS:-linux windows}"
MAX_ATTEMPTS=3

BEPINEX_API="${BEPINEX_API:-https://thunderstore.io/api/experimental/package/denikson/BepInExPack_Valheim/}"

mkdir -p "$STEAMCMD_DIR" "$SERVER_DIR" "$DEPS_DIR"

curl_retry() {
  curl --retry 5 --retry-delay 2 --retry-all-errors "$@"
}

if [ ! -x "$STEAMCMD" ]; then
  echo "Downloading SteamCMD..."
  curl_retry -fsSL https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz \
    | tar -xzf - -C "$STEAMCMD_DIR"
fi

clear_steam_cache() {
  if [ -n "${HOME:-}" ]; then
    rm -rf "$HOME/Steam/appcache"
  fi
  rm -rf "$STEAMCMD_DIR/appcache"
}

install_valheim_server() {
  local -a platforms
  local platform
  local attempt
  local last_exit_code=1
  local managed_dir="$SERVER_DIR/valheim_server_Data/Managed"

  read -r -a platforms <<< "$VALHEIM_STEAM_PLATFORMS"

  for platform in "${platforms[@]}"; do
    for ((attempt = 1; attempt <= MAX_ATTEMPTS; attempt++)); do
      echo "Installing Valheim compile references for Steam platform '$platform' (attempt $attempt/$MAX_ATTEMPTS)..."
      if "$STEAMCMD" \
        +@sSteamCmdForcePlatformType "$platform" \
        +force_install_dir "$(pwd)/$SERVER_DIR" \
        +login anonymous \
        +app_update 896660 validate \
        +quit; then
        last_exit_code=0
      else
        last_exit_code=$?
      fi

      if [ -d "$managed_dir" ]; then
        echo "Valheim dedicated-server references installed for Steam platform '$platform'."
        return 0
      fi

      if [ "$last_exit_code" -eq 0 ]; then
        last_exit_code=1
      fi

      if [ "$attempt" -lt "$MAX_ATTEMPTS" ]; then
        echo "SteamCMD attempt $attempt for '$platform' failed with exit code $last_exit_code; clearing cache and retrying..."
        clear_steam_cache
        sleep $((attempt * 10))
      fi
    done

    if [ "$platform" = "linux" ]; then
      echo "Linux references unavailable; falling back to the Windows depot for compile references only."
      rm -rf "$SERVER_DIR"
      mkdir -p "$SERVER_DIR"
      clear_steam_cache
    fi
  done

  echo "Valheim managed assemblies were not installed after bounded SteamCMD retries." >&2
  return "$last_exit_code"
}

install_valheim_server

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

echo "Reference assemblies ready:"
echo "  Valheim: $SERVER_DIR/valheim_server_Data/Managed"
echo "  BepInEx: $DEPS_DIR/bepinex/BepInExPack_Valheim/BepInEx/core"
