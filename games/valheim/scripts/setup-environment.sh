#!/usr/bin/env bash
# Downloads the Valheim dedicated-server assemblies plus BepInEx reference DLLs
# needed to compile the Valheim connector plugin.
set -euo pipefail
cd "$(dirname "$0")/.."

DATA_DIR="${VALHEIM_DATA_DIR:-_data}"
STEAMCMD_DIR="${STEAMCMD_DIR:-${DATA_DIR}/steamcmd}"
REFERENCE_CACHE_DIR="${VALHEIM_REFERENCE_CACHE_DIR:-${VALHEIM_SERVER_DIR:-${DATA_DIR}/server}}"
SERVER_DIR="$REFERENCE_CACHE_DIR"
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
REFERENCE_CACHE_MARKER_NAME=".takaro-valheim-reference-cache"
REFERENCE_CACHE_MARKER_CONTENT="takaro-valheim-reference-cache-v1"

BEPINEX_API="${BEPINEX_API:-https://thunderstore.io/api/experimental/package/denikson/BepInExPack_Valheim/}"

if ! command -v file >/dev/null 2>&1; then
  echo "Valheim reference setup requires the 'file' command to validate real PE/CLI assemblies." >&2
  exit 1
fi

mkdir -p "$STEAMCMD_DIR" "$(dirname "$SERVER_DIR")" "$DEPS_DIR"

ACTIVE_SERVER_STAGE=""
ACTIVE_SERVER_BACKUP=""
ACTIVE_SERVER_FINAL=""
ACTIVE_STEAMCMD_ARCHIVE=""
ACTIVE_STEAMCMD_STAGE=""
ACTIVE_STEAMCMD_BACKUP=""
ACTIVE_STEAMCMD_FINAL=""
ACTIVE_STEAMCMD_FINAL_WITHOUT_BACKUP=false
STEAMCMD_COMPLETION_MARKER="${STEAMCMD_DIR}/.takaro-steamcmd-complete"

cleanup_steamcmd_publication_state() {
  if [ -n "$ACTIVE_STEAMCMD_BACKUP" ] && [ -e "$ACTIVE_STEAMCMD_BACKUP" ]; then
    if [ -n "$ACTIVE_STEAMCMD_FINAL" ]; then
      if [ -e "$ACTIVE_STEAMCMD_FINAL" ] && ! rm -rf "$ACTIVE_STEAMCMD_FINAL"; then
        echo "Interrupted SteamCMD publication could not remove the uncommitted replacement; the previous install remains preserved at $ACTIVE_STEAMCMD_BACKUP." >&2
      elif mv "$ACTIVE_STEAMCMD_BACKUP" "$ACTIVE_STEAMCMD_FINAL"; then
        ACTIVE_STEAMCMD_BACKUP=""
      else
        echo "Interrupted SteamCMD publication could not restore the previous install; it remains preserved at $ACTIVE_STEAMCMD_BACKUP." >&2
      fi
    fi
  elif [ "$ACTIVE_STEAMCMD_FINAL_WITHOUT_BACKUP" = true ] \
    && [ -n "$ACTIVE_STEAMCMD_FINAL" ] \
    && [ -e "$ACTIVE_STEAMCMD_FINAL" ]; then
    rm -rf "$ACTIVE_STEAMCMD_FINAL"
  fi

  if [ -n "$ACTIVE_STEAMCMD_ARCHIVE" ]; then
    rm -f "$ACTIVE_STEAMCMD_ARCHIVE"
    ACTIVE_STEAMCMD_ARCHIVE=""
  fi
  if [ -n "$ACTIVE_STEAMCMD_STAGE" ]; then
    rm -rf "$ACTIVE_STEAMCMD_STAGE"
    ACTIVE_STEAMCMD_STAGE=""
  fi
  ACTIVE_STEAMCMD_FINAL=""
  ACTIVE_STEAMCMD_FINAL_WITHOUT_BACKUP=false
}

cleanup_server_publication_state() {
  if [ -n "$ACTIVE_SERVER_BACKUP" ] && [ -e "$ACTIVE_SERVER_BACKUP" ]; then
    if [ -n "$ACTIVE_SERVER_FINAL" ]; then
      if [ -e "$ACTIVE_SERVER_FINAL" ] && ! rm -rf "$ACTIVE_SERVER_FINAL"; then
        echo "Interrupted Valheim publication could not remove the uncommitted replacement; the previous install remains preserved at $ACTIVE_SERVER_BACKUP." >&2
      elif mv "$ACTIVE_SERVER_BACKUP" "$ACTIVE_SERVER_FINAL"; then
        ACTIVE_SERVER_BACKUP=""
      else
        echo "Interrupted Valheim publication could not restore the previous install; it remains preserved at $ACTIVE_SERVER_BACKUP." >&2
      fi
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
  cleanup_steamcmd_publication_state
  cleanup_server_publication_state
  trap - EXIT
  exit "$status"
}

trap cleanup_on_exit EXIT
trap 'exit 130' HUP INT TERM

curl_retry() {
  curl --retry 5 --retry-delay 2 --retry-all-errors "$@"
}

publish_steamcmd_install() {
  local stage_dir="$1"
  local final_dir="$2"
  local backup_dir="${final_dir}.backup.$$.${RANDOM}"
  local publish_status

  ACTIVE_STEAMCMD_FINAL="$final_dir"
  ACTIVE_STEAMCMD_FINAL_WITHOUT_BACKUP=false
  if [ -e "$final_dir" ]; then
    ACTIVE_STEAMCMD_BACKUP="$backup_dir"
    if mv "$final_dir" "$backup_dir"; then
      :
    else
      publish_status=$?
      echo "Could not move the existing SteamCMD install aside for atomic publication: $final_dir" >&2
      cleanup_steamcmd_publication_state
      return "$publish_status"
    fi
  else
    ACTIVE_STEAMCMD_FINAL_WITHOUT_BACKUP=true
  fi

  if mv "$stage_dir" "$final_dir"; then
    :
  else
    publish_status=$?
    echo "Could not atomically publish the completed SteamCMD install to $final_dir; restoring the previous state." >&2
    cleanup_steamcmd_publication_state
    return "$publish_status"
  fi

  ACTIVE_STEAMCMD_STAGE=""
  backup_dir="$ACTIVE_STEAMCMD_BACKUP"
  ACTIVE_STEAMCMD_BACKUP=""
  ACTIVE_STEAMCMD_FINAL=""
  ACTIVE_STEAMCMD_FINAL_WITHOUT_BACKUP=false
  if [ -n "$backup_dir" ]; then
    rm -rf "$backup_dir"
  fi
}

install_steamcmd() {
  local steamcmd_archive
  local steamcmd_stage_dir
  local install_status=0

  if ! steamcmd_archive="$(mktemp "${STEAMCMD_DIR}.download.XXXXXX.tar.gz")"; then
    echo "Could not create a sibling temporary SteamCMD archive for $STEAMCMD_DIR." >&2
    return 1
  fi
  ACTIVE_STEAMCMD_ARCHIVE="$steamcmd_archive"
  if ! steamcmd_stage_dir="$(mktemp -d "${STEAMCMD_DIR}.stage.XXXXXX")"; then
    rm -f "$steamcmd_archive"
    ACTIVE_STEAMCMD_ARCHIVE=""
    echo "Could not create a sibling SteamCMD staging directory for $STEAMCMD_DIR." >&2
    return 1
  fi
  ACTIVE_STEAMCMD_STAGE="$steamcmd_stage_dir"

  if [ -d "$STEAMCMD_DIR" ] && ! cp -a "$STEAMCMD_DIR/." "$steamcmd_stage_dir/"; then
    echo "Could not preserve existing files while staging the SteamCMD repair." >&2
    install_status=1
  fi

  if [ "$install_status" -eq 0 ] && ! curl_retry -fsSL \
    https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz \
    -o "$steamcmd_archive"; then
    echo "SteamCMD archive download failed before extraction." >&2
    install_status=1
  elif [ "$install_status" -eq 0 ] && ! tar -xzf "$steamcmd_archive" -C "$steamcmd_stage_dir"; then
    echo "Downloaded SteamCMD archive could not be extracted." >&2
    install_status=1
  elif [ "$install_status" -eq 0 ] && [ ! -x "$steamcmd_stage_dir/steamcmd.sh" ]; then
    echo "Downloaded SteamCMD archive is missing executable steamcmd.sh." >&2
    install_status=1
  elif [ "$install_status" -eq 0 ] && ! : > "$steamcmd_stage_dir/.takaro-steamcmd-complete"; then
    echo "Could not mark the staged SteamCMD install complete." >&2
    install_status=1
  elif [ "$install_status" -eq 0 ] && ! publish_steamcmd_install "$steamcmd_stage_dir" "$STEAMCMD_DIR"; then
    install_status=1
  fi

  cleanup_steamcmd_publication_state
  return "$install_status"
}

steamcmd_install_is_complete() {
  if [ "$STEAMCMD" = "$STEAMCMD_DIR/steamcmd.sh" ]; then
    [ -x "$STEAMCMD" ] && [ -f "$STEAMCMD_COMPLETION_MARKER" ]
  else
    [ -x "$STEAMCMD" ]
  fi
}

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

reference_cache_is_owned() {
  local cache_dir="$1"
  local marker="$cache_dir/$REFERENCE_CACHE_MARKER_NAME"
  local marker_content

  [ -f "$marker" ] || return 1
  IFS= read -r marker_content < "$marker" || return 1
  [ "$marker_content" = "$REFERENCE_CACHE_MARKER_CONTENT" ]
}

reference_cache_has_entries() {
  local cache_dir="$1"
  [ -d "$cache_dir" ] || return 1
  [ -n "$(find -H "$cache_dir" -mindepth 1 -maxdepth 1 -print -quit)" ]
}

ensure_reference_cache_write_is_safe() {
  local cache_dir="$1"

  if [ -e "$cache_dir" ] && [ ! -d "$cache_dir" ]; then
    echo "Valheim reference cache target is not a directory; refusing to mutate it: $cache_dir" >&2
    echo "Set VALHEIM_REFERENCE_CACHE_DIR to a separate empty directory used only for compile references." >&2
    return 1
  fi

  if ! reference_cache_has_entries "$cache_dir"; then
    return 0
  fi

  if reference_cache_is_owned "$cache_dir"; then
    return 0
  fi

  echo "Valheim reference cache target is non-empty and unowned; refusing to mutate it: $cache_dir" >&2
  if [ -e "$cache_dir/valheim_server.x86_64" ]; then
    echo "Detected live-server marker: $cache_dir/valheim_server.x86_64" >&2
  fi
  echo "Validated assemblies may be reused read-only, but invalid caller/live-server files are never replaced." >&2
  echo "Set VALHEIM_REFERENCE_CACHE_DIR to a separate empty directory used only for compile references." >&2
  echo "Legacy VALHEIM_SERVER_DIR remains read-only unless it is empty or already carries the Takaro reference-cache ownership marker." >&2
  return 1
}

ensure_steamcmd_install() {
  if steamcmd_install_is_complete; then
    return 0
  fi

  if [ -x "$STEAMCMD" ] && [ "$STEAMCMD" = "$STEAMCMD_DIR/steamcmd.sh" ]; then
    echo "Repairing markerless or incomplete managed SteamCMD install..."
  fi
  echo "Downloading SteamCMD..."
  install_steamcmd
}

publish_valheim_server_install() {
  local stage_dir="$1"
  local final_dir="$2"
  local backup_dir="${final_dir}.backup.$$.${RANDOM}"
  local publish_status

  if [ -e "$final_dir" ]; then
    # Record rollback state before the first rename so a signal cannot land in
    # the gap after the old install moves but before cleanup knows its paths.
    ACTIVE_SERVER_BACKUP="$backup_dir"
    ACTIVE_SERVER_FINAL="$final_dir"
    if mv "$final_dir" "$backup_dir"; then
      :
    else
      publish_status=$?
      echo "Could not move the existing Valheim install aside for atomic publication: $final_dir" >&2
      cleanup_server_publication_state
      return "$publish_status"
    fi
  fi

  if mv "$stage_dir" "$final_dir"; then
    ACTIVE_SERVER_STAGE=""
    if [ -n "$ACTIVE_SERVER_BACKUP" ]; then
      backup_dir="$ACTIVE_SERVER_BACKUP"
      ACTIVE_SERVER_BACKUP=""
      ACTIVE_SERVER_FINAL=""
      rm -rf "$backup_dir"
    fi
    ACTIVE_SERVER_FINAL=""
    return 0
  else
    publish_status=$?
  fi

  echo "Could not atomically publish validated Valheim references to $final_dir; restoring the previous install." >&2
  cleanup_server_publication_state
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
    echo "Reusing validated Valheim references read-only at $managed_dir."
    return 0
  fi

  if ! ensure_reference_cache_write_is_safe "$server_install_dir"; then
    return 1
  fi

  if ! ensure_steamcmd_install; then
    return 1
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
        if ! printf '%s\n' "$REFERENCE_CACHE_MARKER_CONTENT" > "$stage_dir/$REFERENCE_CACHE_MARKER_NAME"; then
          echo "Could not mark the validated Valheim reference cache complete: $stage_dir" >&2
          last_exit_code=1
        elif publish_valheim_server_install "$stage_dir" "$server_install_dir"; then
          echo "Valheim compile-reference cache installed for Steam platform '$platform'."
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
echo "  Valheim: $REFERENCE_CACHE_DIR/valheim_server_Data/Managed"
echo "  BepInEx: $DEPS_DIR/bepinex/BepInExPack_Valheim/BepInEx/core"
