#!/usr/bin/env bash
# Builds TakaroEmpyrion.dll for local testing against the real Empyrion dedicated-server assemblies.
# Output: bin/TakaroEmpyrion/ (the folder that goes into <server>/Content/Mods/).
#
# By default it compiles in the catalog target's pinned mono image against the target's pinned
# Mif.dll, ModApi.dll and Newtonsoft.Json.dll (fetched once into _data/refs/<fingerprint>/).
# Overrides: EMPYRION_MANAGED=<server>/DedicatedServer/EmpyrionDedicated_Data/Managed compiles against a
# server's own folder instead, BUILDER_IMAGE=<image> uses another image, --target <id> another target.
# A release zip comes from build-release.sh, not from here.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib-target.sh
. "$here/scripts/lib-target.sh"
empyrion_parse_target_flag "$@"
if [ -z "${EMPYRION_MANAGED:-}" ] || [ -z "${BUILDER_IMAGE:-}" ]; then
  empyrion_resolve_target "$TARGET"
fi
if [ -z "${EMPYRION_MANAGED:-}" ]; then
  EMPYRION_MANAGED="$here/_data/refs/$EMPYRION_FP16"
  empyrion_references "$EMPYRION_MANAGED"
fi
managed="$(cd "$EMPYRION_MANAGED" && pwd)"
image="${BUILDER_IMAGE:-$EMPYRION_TOOLCHAIN}"
out="$here/bin/TakaroEmpyrion"
rm -rf "$out" && mkdir -p "$out"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$here:/src" -v "$managed:/managed:ro" -w /src "$image" bash -c '
  set -euo pipefail
  mapfile -t sources < <(find mod/src -name "*.cs" | LC_ALL=C sort)
  csc -nologo -langversion:7.3 -optimize+ -deterministic -target:library -out:bin/TakaroEmpyrion/TakaroEmpyrion.dll \
    -r:/managed/Mif.dll -r:/managed/ModApi.dll -r:/managed/Newtonsoft.Json.dll \
    -r:/usr/lib/mono/4.5/Facades/netstandard.dll -r:System.Net.Http.dll -r:System.IO.Compression.dll \
    -resource:assets/icon-256.png,TakaroEmpyrion.icon-256.png "${sources[@]}"'
grep -v "^# x-release-please-" "$here/mod/TakaroEmpyrion_Info.yaml" > "$out/TakaroEmpyrion_Info.yaml"
cp "$here/TakaroConfig.txt.example" "$out/"
sha256sum "$out/TakaroEmpyrion.dll"
