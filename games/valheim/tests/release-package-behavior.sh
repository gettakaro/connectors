#!/usr/bin/env bash
# What a Valheim release has to be true of, checked against the archives themselves.
#
# Two roles, two archives -- the dedicated-server plugin and the optional client inventory
# companion -- and the rule that matters: neither may carry the other's own assemblies (both
# ship the shared Takaro.Valheim.Companion.Protocol.dll), and the client may carry nothing
# that talks to Takaro. The archives are named by the target record, so this also checks the
# names, the .meta.json sidecar each one carries, the wire protocol version and the three
# different BepInEx-shaped numbers in manifest.json.
#
# Usage: release-package-behavior.sh <version> <dist-dir>
#   VALHEIM_TARGET_ID, VALHEIM_TARGET_FINGERPRINT   when set, the names and sidecars are
#                                                   required to match that target
#   VALHEIM_BEPINEX_VERSION_EXPECTED                when set, the pinned pack version
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

# Exactly one archive per role. The name comes from the catalog target, so it is matched
# by its role prefix rather than spelled out here; two of either is a packaging bug, and a
# release that shipped both would be ambiguous about which bytes it meant.
one_archive() {
  local role_glob="$1" role="$2"
  local -a found=()
  while IFS= read -r candidate; do
    found+=("$candidate")
  done < <(find "$dist_dir" -maxdepth 1 -type f -name "$role_glob" -print | LC_ALL=C sort)
  if [ "${#found[@]}" -ne 1 ]; then
    printf 'expected exactly one %s archive in %s, found %d\n' "$role" "$dist_dir" "${#found[@]}" >&2
    exit 1
  fi
  printf '%s\n' "${found[0]}"
}

# takaro-valheim-plugin.zip and takaro-valheim-inventory-companion.zip are the legacy names;
# the publisher still ships them as aliases of the target-named archives.
server_zip="$(one_archive 'takaro-valheim-plugin*.zip' 'server plugin')"
client_zip="$(one_archive 'takaro-valheim-inventory-companion*.zip' 'client companion')"

# When the build knows its target, the archive names and their sidecars have to say so:
# an archive nobody can place against a target cannot be published as evidence of one.
check_target_identity() {
  local archive="$1" role="$2" expected_name meta
  [ -n "${VALHEIM_TARGET_ID:-}" ] || return 0
  expected_name="takaro-valheim-${role}-${VALHEIM_TARGET_ID}-${VALHEIM_RELEASE_VERSION}.zip"
  if [ "$(basename "$archive")" != "$expected_name" ]; then
    printf 'release archive is not named for its target: %s (expected %s)\n' "$archive" "$expected_name" >&2
    exit 1
  fi
  meta="${archive}.meta.json"
  [ -f "$meta" ] || {
    printf 'release archive has no .meta.json sidecar: %s\n' "$meta" >&2
    exit 1
  }
  jq -e \
    --arg target "$VALHEIM_TARGET_ID" \
    --arg fingerprint "${VALHEIM_TARGET_FINGERPRINT:-}" \
    --arg role "$3" \
    --arg version "$VALHEIM_RELEASE_VERSION" \
    '.target == $target
      and .role == $role
      and .connectorVersion == $version
      and ($fingerprint == "" or .fingerprint == $fingerprint)' \
    "$meta" >/dev/null || {
      printf 'release archive sidecar does not describe this target: %s\n' "$meta" >&2
      exit 1
    }
}

check_target_identity "$server_zip" plugin server-plugin
check_target_identity "$client_zip" inventory-companion client-companion

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
client_dir="$client_extract/TakaroValheimInventoryCompanion"
[ -d "$server_dir" ] || {
  printf 'server archive is missing TakaroValheim root\n' >&2
  exit 1
}
[ -d "$client_dir" ] || {
  printf 'client archive is missing TakaroValheimInventoryCompanion root\n' >&2
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
  "$server_dir/takaro.cfg" \
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

# The server ships exactly one config, takaro.cfg, the file people edit, and it must not
# carry a registration token or an identity: every install gets its own.
while IFS= read -r packaged_file; do
  packaged_name="$(basename "$packaged_file")"
  [ "$packaged_file" = "$server_dir/takaro.cfg" ] && continue
  case "$packaged_name" in
    *.pdb|*.deps.json|*.runtimeconfig.json|*.exe|*.cfg|*.config|0Harmony.dll|BepInEx.dll|assembly_valheim.dll|assembly_utils.dll|Splatform.dll|UnityEngine.dll|UnityEngine.*.dll|Jotunn.dll|ServerSync.dll)
      printf 'release archive contains forbidden debug, config, or host file: %s\n' "$packaged_file" >&2
      exit 1
      ;;
  esac
done < <(find "$server_extract" "$client_extract" -type f -print)

for key in registrationToken identityToken; do
  if ! rg -q "^${key} =\s*$" "$server_dir/takaro.cfg"; then
    printf 'shipped takaro.cfg must have an empty %s line\n' "$key" >&2
    exit 1
  fi
done

if find "$server_extract" "$client_extract" -type l -print -quit | rg -q .; then
  printf 'release archive contains a symbolic link\n' >&2
  exit 1
fi

# manifest.json states three different BepInEx-shaped numbers and they are not
# interchangeable: `pluginVersion` is what [BepInPlugin] declares (this connector's own
# numeric core), `bepInExPack.version` is the Thunderstore package the target pins, and
# `bepInExVersion` is the loader assembly's own version. A manifest that repeats the plugin
# version as the loader version is wrong.
validate_manifest() {
  local manifest="$1"
  local expected_name="$2"
  local expected_role="$3"
  jq -e \
    --arg name "$expected_name" \
    --arg version "$VALHEIM_RELEASE_VERSION" \
    --arg plugin "$VALHEIM_BEPINEX_VERSION" \
    --arg role "$expected_role" \
    --arg pack "${VALHEIM_BEPINEX_VERSION_EXPECTED:-}" \
    --arg target "${VALHEIM_TARGET_ID:-}" \
    --arg fingerprint "${VALHEIM_TARGET_FINGERPRINT:-}" \
    '.name == $name
      and .productVersion == $version
      and .pluginVersion == $plugin
      and .processRole == $role
      and .protocol.minimum == 3
      and .protocol.current == 3
      and .protocol.maximum == 3
      and .bepInExPack.namespace == "denikson"
      and .bepInExPack.name == "BepInExPack_Valheim"
      and (.bepInExPack.version | test("^[0-9]+\\.[0-9]+\\.[0-9]+$"))
      and ($pack == "" or .bepInExPack.version == $pack)
      and (.bepInExVersion | test("^[0-9]+(\\.[0-9]+){1,3}$"))
      and .bepInExVersion != .pluginVersion
      and ($target == "" or .target.id == $target)
      and ($fingerprint == "" or .target.fingerprint == $fingerprint)' \
    "$manifest" >/dev/null || {
      printf 'release manifest does not match product, role, protocol or BepInEx contract: %s\n' "$manifest" >&2
      exit 1
    }
}

validate_manifest "$server_dir/manifest.json" "TakaroValheim" "dedicated-server"
validate_manifest "$client_dir/manifest.json" "TakaroValheimInventoryCompanion" "graphical-client"

for marker in registrationToken identityToken takaroWsUrl connect.takaro.io \
  ClientWebSocket TakaroWebSocketRunner ValheimServerAdapter; do
  if rg -a -q "$marker" "$client_extract/TakaroValheimInventoryCompanion"; then
    printf 'client artifact contains banned marker: %s\n' "$marker" >&2
    exit 1
  fi
done

printf 'Valheim server and client release package behavior is valid.\n'
