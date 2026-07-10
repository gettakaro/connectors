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
REQUIRED_VALHEIM_ASSEMBLIES=(
  assembly_valheim.dll
  assembly_utils.dll
  Splatform.dll
  UnityEngine.dll
  UnityEngine.CoreModule.dll
)
REQUIRED_BEPINEX_ASSEMBLIES=(
  BepInEx.dll
  0Harmony.dll
)

BEPINEX_API="${BEPINEX_API:-https://thunderstore.io/api/experimental/package/denikson/BepInExPack_Valheim/}"

if ! command -v file >/dev/null 2>&1; then
  echo "Valheim reference setup requires the 'file' command to validate real PE/CLI assemblies." >&2
  exit 1
fi

mkdir -p "$STEAMCMD_DIR" "$(dirname "$SERVER_DIR")" "$DEPS_DIR"

ACTIVE_SERVER_STAGE=""
ACTIVE_SERVER_BACKUP=""
ACTIVE_SERVER_FINAL=""

cleanup_server_publication_state() {
  if [ -n "$ACTIVE_SERVER_BACKUP" ]; then
    if [ -n "$ACTIVE_SERVER_FINAL" ] && [ ! -e "$ACTIVE_SERVER_FINAL" ]; then
      if mv "$ACTIVE_SERVER_BACKUP" "$ACTIVE_SERVER_FINAL"; then
        ACTIVE_SERVER_BACKUP=""
      else
        echo "Interrupted Valheim publication could not restore the previous install; it remains preserved at $ACTIVE_SERVER_BACKUP." >&2
      fi
    else
      echo "Interrupted Valheim publication preserved the previous install backup at $ACTIVE_SERVER_BACKUP." >&2
    fi
  fi
  if [ -n "$ACTIVE_SERVER_STAGE" ]; then
    rm -rf "$ACTIVE_SERVER_STAGE"
    ACTIVE_SERVER_STAGE=""
  fi
  ACTIVE_SERVER_FINAL=""
}

cleanup_on_exit() {
  local status=$?
  cleanup_server_publication_state
  trap - EXIT
  exit "$status"
}

trap cleanup_on_exit EXIT
trap 'exit 130' HUP INT TERM

curl_retry() {
  curl --retry 5 --retry-delay 2 --retry-all-errors "$@"
}

install_steamcmd() {
  local steamcmd_archive
  local steamcmd_extract_dir
  local install_status=0

  if ! steamcmd_archive="$(mktemp "$STEAMCMD_DIR/steamcmd.XXXXXX.tar.gz")"; then
    echo "Could not create a temporary SteamCMD archive under $STEAMCMD_DIR." >&2
    return 1
  fi
  if ! steamcmd_extract_dir="$(mktemp -d "$STEAMCMD_DIR/steamcmd-extract.XXXXXX")"; then
    rm -f "$steamcmd_archive"
    echo "Could not create a temporary SteamCMD extraction directory under $STEAMCMD_DIR." >&2
    return 1
  fi

  if ! curl_retry -fsSL \
    https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz \
    -o "$steamcmd_archive"; then
    echo "SteamCMD archive download failed before extraction." >&2
    install_status=1
  elif ! tar -xzf "$steamcmd_archive" -C "$steamcmd_extract_dir"; then
    echo "Downloaded SteamCMD archive could not be extracted." >&2
    install_status=1
  elif [ ! -x "$steamcmd_extract_dir/steamcmd.sh" ]; then
    echo "Downloaded SteamCMD archive is missing executable steamcmd.sh." >&2
    install_status=1
  elif ! cp -a "$steamcmd_extract_dir/." "$STEAMCMD_DIR/"; then
    echo "Downloaded SteamCMD files could not be published under $STEAMCMD_DIR." >&2
    install_status=1
  fi

  rm -f "$steamcmd_archive"
  rm -rf "$steamcmd_extract_dir"
  return "$install_status"
}

if [ ! -x "$STEAMCMD" ]; then
  echo "Downloading SteamCMD..."
  if ! install_steamcmd; then
    exit 1
  fi
fi

clear_steam_cache() {
  if [ -n "${HOME:-}" ]; then
    rm -rf "$HOME/Steam/appcache"
  fi
  rm -rf "$STEAMCMD_DIR/appcache"
}

validate_managed_assemblies() {
  local managed_dir="$1"
  local assembly

  [ -d "$managed_dir" ] || return 1
  for assembly in "${REQUIRED_VALHEIM_ASSEMBLIES[@]}"; do
    if ! is_managed_pe_assembly "$managed_dir/$assembly"; then
      echo "SteamCMD required Valheim assembly is not a managed PE/CLI assembly: $managed_dir/$assembly" >&2
      return 1
    fi
  done
}

validate_bepinex_assemblies() {
  local core_dir="$1"
  local assembly

  [ -d "$core_dir" ] || return 1
  for assembly in "${REQUIRED_BEPINEX_ASSEMBLIES[@]}"; do
    if ! is_managed_pe_assembly "$core_dir/$assembly"; then
      echo "Downloaded required BepInEx assembly is not a managed PE/CLI assembly: $core_dir/$assembly" >&2
      return 1
    fi
  done
}

is_managed_pe_assembly() {
  local assembly_path="$1"
  local description

  [ -f "$assembly_path" ] || return 1
  if ! description="$(LC_ALL=C file -b -- "$assembly_path")"; then
    return 1
  fi

  case "$description" in
    *PE32*Mono/.Net\ assembly*) return 0 ;;
    *) return 1 ;;
  esac
}

publish_valheim_server_install() {
  local stage_dir="$1"
  local final_dir="$2"
  local backup_dir="${final_dir}.backup.$$.${RANDOM}"
  local publish_status

  if [ -e "$final_dir" ]; then
    if ! mv "$final_dir" "$backup_dir"; then
      echo "Could not move the existing Valheim install aside for atomic publication: $final_dir" >&2
      return 1
    fi
    ACTIVE_SERVER_BACKUP="$backup_dir"
    ACTIVE_SERVER_FINAL="$final_dir"
  fi

  if mv "$stage_dir" "$final_dir"; then
    ACTIVE_SERVER_STAGE=""
    if [ -n "$ACTIVE_SERVER_BACKUP" ]; then
      rm -rf "$ACTIVE_SERVER_BACKUP"
      ACTIVE_SERVER_BACKUP=""
    fi
    ACTIVE_SERVER_FINAL=""
    return 0
  else
    publish_status=$?
  fi

  echo "Could not atomically publish validated Valheim references to $final_dir; restoring the previous install." >&2
  if [ -n "$ACTIVE_SERVER_BACKUP" ]; then
    if mv "$ACTIVE_SERVER_BACKUP" "$final_dir"; then
      ACTIVE_SERVER_BACKUP=""
      ACTIVE_SERVER_FINAL=""
    else
      echo "Rollback failed; the preserved previous install remains at $ACTIVE_SERVER_BACKUP." >&2
    fi
  fi
  return "$publish_status"
}

install_valheim_server() {
  local -a platforms
  local platform
  local attempt
  local last_exit_code=1
  local managed_dir="$SERVER_DIR/valheim_server_Data/Managed"
  local server_install_dir="$SERVER_DIR"
  local stage_dir
  local stage_managed_dir

  read -r -a platforms <<< "$VALHEIM_STEAM_PLATFORMS"
  if [ "${#platforms[@]}" -eq 0 ]; then
    echo "VALHEIM_STEAM_PLATFORMS must name at least one Steam platform." >&2
    return 1
  fi

  if [[ "$server_install_dir" != /* ]]; then
    server_install_dir="$(pwd)/$server_install_dir"
    managed_dir="$server_install_dir/valheim_server_Data/Managed"
  fi

  if validate_managed_assemblies "$managed_dir"; then
    echo "Reusing validated Valheim dedicated-server references at $managed_dir."
    return 0
  fi

  for platform in "${platforms[@]}"; do
    for ((attempt = 1; attempt <= MAX_ATTEMPTS; attempt++)); do
      if ! stage_dir="$(mktemp -d "${server_install_dir}.stage.XXXXXX")"; then
        echo "Could not create a sibling Valheim staging directory for $server_install_dir." >&2
        return 1
      fi
      ACTIVE_SERVER_STAGE="$stage_dir"
      if [ -d "$server_install_dir" ] && ! cp -a "$server_install_dir/." "$stage_dir/"; then
        echo "Could not stage the existing Valheim install before update: $server_install_dir" >&2
        return 1
      fi
      stage_managed_dir="$stage_dir/valheim_server_Data/Managed"

      echo "Installing Valheim compile references for Steam platform '$platform' (attempt $attempt/$MAX_ATTEMPTS)..."
      if "$STEAMCMD" \
        +@sSteamCmdForcePlatformType "$platform" \
        +force_install_dir "$stage_dir" \
        +login anonymous \
        +app_update 896660 validate \
        +quit; then
        last_exit_code=0
      else
        last_exit_code=$?
      fi

      if [ "$last_exit_code" -eq 0 ] && validate_managed_assemblies "$stage_managed_dir"; then
        if publish_valheim_server_install "$stage_dir" "$server_install_dir"; then
          echo "Valheim dedicated-server references installed for Steam platform '$platform'."
          return 0
        fi
        last_exit_code=1
      fi

      if [ "$last_exit_code" -eq 0 ]; then
        last_exit_code=1
      fi

      if [ -n "$ACTIVE_SERVER_STAGE" ]; then
        rm -rf "$ACTIVE_SERVER_STAGE"
        ACTIVE_SERVER_STAGE=""
      fi

      if [ "$attempt" -lt "$MAX_ATTEMPTS" ]; then
        echo "SteamCMD attempt $attempt for '$platform' failed with exit code $last_exit_code; clearing cache and retrying..."
        clear_steam_cache
        sleep $((attempt * 10))
      fi
    done

    if [ "$platform" = "linux" ]; then
      echo "Linux references unavailable; falling back to the Windows depot for compile references only."
      clear_steam_cache
    fi
  done

  echo "Valheim managed assemblies were not installed after bounded SteamCMD retries." >&2
  echo "Expected managed directory: $managed_dir" >&2
  echo "Attempted Steam platforms: ${platforms[*]}" >&2
  echo "Set VALHEIM_STEAM_PLATFORMS to override the compile-reference platform order (default: linux windows)." >&2
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
validate_bepinex_assemblies "$DEPS_DIR/bepinex/BepInExPack_Valheim/BepInEx/core"

echo "Reference assemblies ready:"
echo "  Valheim: $SERVER_DIR/valheim_server_Data/Managed"
echo "  BepInEx: $DEPS_DIR/bepinex/BepInExPack_Valheim/BepInEx/core"
