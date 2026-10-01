#!/usr/bin/env bash
# What a Valheim release has to be true of, checked against the archives themselves.
#
# One role, one archive: the dedicated-server plugin. There is no client-side component, so
# any companion archive or assembly is a packaging bug. The archive is named by the target
# record, so this also checks the name, the .meta.json sidecar it carries, and the three
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

# Exactly one plugin archive. The name comes from the catalog target, so it is matched by
# its role prefix rather than spelled out here; two is a packaging bug, and a release that
# shipped both would be ambiguous about which bytes it meant.
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

# takaro-valheim-plugin.zip is the legacy name; the publisher still ships it as an alias
# of the target-named archive.
server_zip="$(one_archive 'takaro-valheim-plugin*.zip' 'server plugin')"

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

if zipinfo -1 "$server_zip" | rg -q '(^/|(^|/)\.\.(/|$))'; then
  printf 'release archive contains an unsafe path: %s\n' "$server_zip" >&2
  exit 1
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
server_extract="$work_dir/server"
mkdir -p "$server_extract"
unzip -q "$server_zip" -d "$server_extract"

server_dir="$server_extract/TakaroValheim"
[ -d "$server_dir" ] || {
  printf 'server archive is missing TakaroValheim root\n' >&2
  exit 1
}

if [ "$(find "$server_extract" -mindepth 1 -maxdepth 1 | wc -l)" -ne 1 ]; then
  printf 'server archive contains unexpected top-level entries\n' >&2
  exit 1
fi

for required in \
  "$server_dir/TakaroValheim.dll" \
  "$server_dir/Takaro.Valheim.Core.dll" \
  "$server_dir/README.txt" \
  "$server_dir/manifest.json"; do
  [ -f "$required" ] || {
    printf 'release archive is missing required file: %s\n' "$required" >&2
    exit 1
  }
done

# Nothing of the retired client companion may ride along in the server package.
if find "$server_extract" -type f -iname '*Companion*' -print -quit | rg -q .; then
  printf 'release archive contains a client companion file\n' >&2
  exit 1
fi

while IFS= read -r packaged_file; do
  packaged_name="$(basename "$packaged_file")"
  case "$packaged_name" in
    *.pdb|*.deps.json|*.runtimeconfig.json|*.exe|*.cfg|*.config|0Harmony.dll|BepInEx.dll|assembly_valheim.dll|assembly_utils.dll|Splatform.dll|UnityEngine.dll|UnityEngine.*.dll|Jotunn.dll|ServerSync.dll)
      printf 'release archive contains forbidden debug, config, or host file: %s\n' "$packaged_file" >&2
      exit 1
      ;;
  esac
done < <(find "$server_extract" -type f -print)

if find "$server_extract" -type l -print -quit | rg -q .; then
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
      and (has("protocol") | not)
      and .bepInExPack.namespace == "denikson"
      and .bepInExPack.name == "BepInExPack_Valheim"
      and (.bepInExPack.version | test("^[0-9]+\\.[0-9]+\\.[0-9]+$"))
      and ($pack == "" or .bepInExPack.version == $pack)
      and (.bepInExVersion | test("^[0-9]+(\\.[0-9]+){1,3}$"))
      and .bepInExVersion != .pluginVersion
      and ($target == "" or .target.id == $target)
      and ($fingerprint == "" or .target.fingerprint == $fingerprint)' \
    "$manifest" >/dev/null || {
      printf 'release manifest does not match product, role or BepInEx contract: %s\n' "$manifest" >&2
      exit 1
    }
}

validate_manifest "$server_dir/manifest.json" "TakaroValheim" "dedicated-server"
printf 'Valheim server plugin release package behavior is valid.\n'
