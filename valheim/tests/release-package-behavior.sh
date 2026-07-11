#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

version="${1:?usage: release-package-behavior.sh <version> <dist-dir>}"
dist_dir="${2:?usage: release-package-behavior.sh <version> <dist-dir>}"
source scripts/release-version.sh
resolve_valheim_release_version "$version" >/dev/null || {
  printf 'invalid expected release version: %s\n' "$version" >&2
  exit 2
}

for command in unzip zipinfo jq rg find; do
  command -v "$command" >/dev/null 2>&1 || {
    printf 'required package validation command is missing: %s\n' "$command" >&2
    exit 2
  }
done

server_zip="$dist_dir/takaro-valheim-plugin.zip"
client_zip="$dist_dir/takaro-valheim-companion.zip"
for archive in "$server_zip" "$client_zip"; do
  [ -f "$archive" ] || {
    printf 'required release archive is missing: %s\n' "$archive" >&2
    exit 1
  }
  if zipinfo -1 "$archive" | rg -q '(^/|(^|/)\.\.(/|$))'; then
    printf 'release archive contains an unsafe path: %s\n' "$archive" >&2
    exit 1
  fi
done

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
server_extract="$work_dir/server"
client_extract="$work_dir/client"
mkdir -p "$server_extract" "$client_extract"
unzip -q "$server_zip" -d "$server_extract"
unzip -q "$client_zip" -d "$client_extract"

server_dir="$server_extract/TakaroValheim"
client_dir="$client_extract/TakaroValheimCompanion"
[ -d "$server_dir" ] || {
  printf 'server archive is missing TakaroValheim root\n' >&2
  exit 1
}
[ -d "$client_dir" ] || {
  printf 'client archive is missing TakaroValheimCompanion root\n' >&2
  exit 1
}

if [ "$(find "$server_extract" -mindepth 1 -maxdepth 1 | wc -l)" -ne 1 ]; then
  printf 'server archive contains unexpected top-level entries\n' >&2
  exit 1
fi
if [ "$(find "$client_extract" -mindepth 1 -maxdepth 1 | wc -l)" -ne 1 ]; then
  printf 'client archive contains unexpected top-level entries\n' >&2
  exit 1
fi

for required in \
  "$server_dir/TakaroValheim.dll" \
  "$server_dir/Takaro.Valheim.Core.dll" \
  "$server_dir/Takaro.Valheim.Companion.Protocol.dll" \
  "$server_dir/README.txt" \
  "$server_dir/manifest.json" \
  "$client_dir/Takaro.Valheim.Companion.dll" \
  "$client_dir/Takaro.Valheim.Companion.Protocol.dll" \
  "$client_dir/README.txt" \
  "$client_dir/manifest.json"; do
  [ -f "$required" ] || {
    printf 'release archive is missing required file: %s\n' "$required" >&2
    exit 1
  }
done

for forbidden in \
  "$server_dir/Takaro.Valheim.Companion.dll" \
  "$client_dir/TakaroValheim.dll" \
  "$client_dir/Takaro.Valheim.Core.dll"; do
  [ ! -e "$forbidden" ] || {
    printf 'release archive contains wrong-role file: %s\n' "$forbidden" >&2
    exit 1
  }
done

while IFS= read -r packaged_file; do
  packaged_name="$(basename "$packaged_file")"
  case "$packaged_name" in
    *.pdb|*.deps.json|*.runtimeconfig.json|*.exe|*.cfg|*.config|0Harmony.dll|BepInEx.dll|assembly_valheim.dll|assembly_utils.dll|Splatform.dll|UnityEngine.dll|UnityEngine.*.dll|Jotunn.dll|ServerSync.dll)
      printf 'release archive contains forbidden debug, config, or host file: %s\n' "$packaged_file" >&2
      exit 1
      ;;
  esac
done < <(find "$server_extract" "$client_extract" -type f -print)

if find "$server_extract" "$client_extract" -type l -print -quit | rg -q .; then
  printf 'release archive contains a symbolic link\n' >&2
  exit 1
fi

validate_manifest() {
  local manifest="$1"
  local expected_name="$2"
  local expected_role="$3"
  jq -e \
    --arg name "$expected_name" \
    --arg version "$VALHEIM_RELEASE_VERSION" \
    --arg loader "$VALHEIM_BEPINEX_VERSION" \
    --arg role "$expected_role" \
    '.name == $name
      and .productVersion == $version
      and .bepInExVersion == $loader
      and .processRole == $role
      and .protocol.minimum == 1
      and .protocol.current == 1
      and .protocol.maximum == 1' \
    "$manifest" >/dev/null || {
      printf 'release manifest does not match product, role, or protocol contract: %s\n' "$manifest" >&2
      exit 1
    }
}

validate_manifest "$server_dir/manifest.json" "TakaroValheim" "dedicated-server"
validate_manifest "$client_dir/manifest.json" "TakaroValheimCompanion" "graphical-client"

for marker in registrationToken identityToken takaroWsUrl connect.takaro.io \
  ClientWebSocket TakaroWebSocketRunner ValheimServerAdapter; do
  if rg -a -q "$marker" "$client_extract/TakaroValheimCompanion"; then
    printf 'client artifact contains banned marker: %s\n' "$marker" >&2
    exit 1
  fi
done

printf 'Valheim server and client release package behavior is valid.\n'
