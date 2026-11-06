#!/usr/bin/env bash
# Builds and runs the contract tests (fake Takaro + fake Empyrion) under Mono.
#
# By default it runs in the catalog target's pinned mono image against the target's pinned
# Mif.dll, ModApi.dll and Newtonsoft.Json.dll (fetched once into _data/refs/<fingerprint>/).
# Overrides: EMPYRION_MANAGED=<server>/DedicatedServer/EmpyrionDedicated_Data/Managed tests against a
# server's own folder instead, BUILDER_IMAGE=<image> uses another image, --target <id> another target.
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
mkdir -p "$here/bin/tests"
# Optional: EMPYRION_SAVES=<server>/Saves (read-only) adds the real-data map checks and writes a reference
# render of every cached planet to bin/tests/map-<planet>.png.
saves=()
if [ -n "${EMPYRION_SAVES:-}" ]; then saves=(-v "$EMPYRION_SAVES:/saves:ro" -e EMPYRION_SAVES=/saves -e EMPYRION_GAME="${EMPYRION_GAME:-}"); fi
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$here:/src" -v "$managed:/managed:ro" "${saves[@]}" -w /src "$image" bash -c '
  set -euo pipefail
  cp /managed/Mif.dll /managed/ModApi.dll /managed/Newtonsoft.Json.dll bin/tests/
  mapfile -t sources < <(find mod/src tests -name "*.cs" | LC_ALL=C sort)
  csc -nologo -langversion:7.3 -target:exe -out:bin/tests/contract-tests.exe \
    -r:/managed/Mif.dll -r:/managed/ModApi.dll -r:/managed/Newtonsoft.Json.dll \
    -r:/usr/lib/mono/4.5/Facades/netstandard.dll -r:System.Net.Http.dll -r:System.IO.Compression.dll \
    "${sources[@]}"
  mono bin/tests/contract-tests.exe capabilities.json'
