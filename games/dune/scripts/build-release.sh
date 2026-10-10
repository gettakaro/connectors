#!/usr/bin/env bash
# Builds the Takaro Dune: Awakening release artifacts for one catalog target and packages
# them deterministically as <out-dir>/<catalog artifact name>:
#
#   takaro-dune-sidecar-<target>-<version>.tar.gz   the connector (REQUIRED)
#   takaro-dune-plugin-<target>-<version>.tar.gz    the LD_PRELOAD plugin (OPTIONAL)
#   SHA256SUMS                                      checksums of everything above
#
# Usage: build-release.sh <version> <out-dir> [--target <catalog target id>]
#
# The host needs neither Node nor a C++ toolchain: both halves are built inside the image
# the catalog pins as this target's toolchain. node:*-bookworm (not slim) is that image
# precisely because it carries g++ on the same glibc as the Funcom server image -- a plugin
# linked against a newer one could not be preloaded into the map server.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
REPO_ROOT=$(cd -- "${PROJECT_ROOT}/../.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

VERSION="${1:?usage: build-release.sh <version> <out-dir> [--target <id>]}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir> [--target <id>]}"
shift 2
dune_parse_target_flag "$@"
dune_resolve_target "${TARGET}"

# The version reaches a file name, a JSON document and a command inside the builder
# container, so it is checked once here rather than escaped three times.
case "${VERSION}" in
  *[!A-Za-z0-9._+-]*|"")
    echo "refusing version '${VERSION}': use letters, digits and . _ + - only" >&2
    exit 2
    ;;
esac

mkdir -p "${OUT_DIR}"
OUT_DIR=$(cd -- "${OUT_DIR}" && pwd)
SIDECAR_ARTIFACT="${DUNE_ARTIFACT_SIDECAR/\{version\}/${VERSION}}"
PLUGIN_ARTIFACT="${DUNE_ARTIFACT_PLUGIN/\{version\}/${VERSION}}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${REPO_ROOT}" log -1 --format=%ct)}"
export TZ=UTC
SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"

# Every dependency the resolved target declares, discovered rather than listed here: a
# dependency added to the catalog must not be able to slip past the check below because
# somebody forgot to add a line to this script.
DEP_ARGS=()
while IFS='=' read -r key _; do
    case "$key" in
        DUNE_DEP_*) DEP_ARGS+=(-e "$key") ;;
    esac
done < <(env)
[ ${#DEP_ARGS[@]} -gt 0 ] || { echo "the resolved target declares no build dependencies" >&2; exit 2; }

# Built in the pinned toolchain image, mounted at its own path so every path inside the
# container is the path outside it. The dependency check runs first: `npm ci` would
# otherwise install whatever the lockfile points at, recorded or not.
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e npm_config_cache=/tmp/npm-cache \
    -e SOURCE_DATE_EPOCH \
    -e TZ \
    "${DEP_ARGS[@]}" \
    -v "${REPO_ROOT}:${REPO_ROOT}" \
    -w "${REPO_ROOT}/games/dune" \
    "${DUNE_TOOLCHAIN}" \
    bash -c 'set -eu
        cd sidecar
        node ../scripts/check-exact-source.mjs
        rm -rf dist node_modules
        npm ci --no-audit --no-fund
        npm run typecheck
        npm test
        npm run build
        cd ../mod
        ./build.sh --native --tests'

# shellcheck source=../../../scripts/lib/package.sh
. "${REPO_ROOT}/scripts/lib/package.sh"

STAGE="${PROJECT_ROOT}/_data/build/stage-${DUNE_FP16}"
rm -rf "${STAGE}"
mkdir -p "${STAGE}"

# dune_stamp <folder> — the identity the running component logs and `deploy` reads.
dune_stamp() {
    cat > "${1}/takaro-target.json" <<JSON
{
  "target": "${DUNE_TARGET}",
  "fingerprint": "${DUNE_FINGERPRINT}",
  "game": "dune",
  "platform": "linux",
  "revision": "${DUNE_REVISION}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${SOURCE_REVISION}"
}
JSON
}

# dune_meta <artifact> — the identity beside the archive, because a tarball has no manifest
# to stamp. `takaro-maint artifact validate` reads this file.
dune_meta() {
    cat > "${OUT_DIR}/${1}.meta.json" <<JSON
{
  "target": "${DUNE_TARGET}",
  "fingerprint": "${DUNE_FINGERPRINT}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${SOURCE_REVISION}",
  "game": "dune",
  "platform": "linux",
  "revision": "${DUNE_REVISION}"
}
JSON
}

# ── the sidecar: the connector itself ────────────────────────────────────────
SPKG="${STAGE}/TakaroDuneSidecar"
mkdir -p "${SPKG}"
cp -R "${PROJECT_ROOT}/sidecar/dist" \
      "${PROJECT_ROOT}/sidecar/scripts" \
      "${PROJECT_ROOT}/sidecar/data" "${SPKG}/"
cp "${PROJECT_ROOT}/sidecar/package.json" "${PROJECT_ROOT}/sidecar/package-lock.json" \
   "${PROJECT_ROOT}/sidecar/Dockerfile" "${PROJECT_ROOT}/sidecar/docker-entrypoint.sh" \
   "${PROJECT_ROOT}/sidecar/.env.example" "${PROJECT_ROOT}/docker-compose.example.yml" "${SPKG}/"
# Not the source tree's .dockerignore: that one excludes dist/ for Dockerfile.dev, which builds it,
# and the release Dockerfile COPYs the prebuilt dist/ instead.
cp "${SCRIPT_DIR}/templates/sidecar.dockerignore.release" "${SPKG}/.dockerignore"
if grep -qx 'dist/\{0,1\}' "${SPKG}/.dockerignore"; then
  echo "build-release: the sidecar package's .dockerignore excludes dist/, which its Dockerfile copies" >&2
  exit 1
fi
rm -rf "${SPKG}/dist/__tests__" "${SPKG}/dist/testing"

# The release must be runnable with `npm ci --omit=dev`, so the entrypoint package.json
# points at has to exist in the packaged dist/.
[ -f "${SPKG}/dist/index.js" ] || { echo "build-release: missing dist/index.js in the sidecar package" >&2; exit 1; }

# The catalogue is generated, never shipped: make sure a local `npm run catalogue` has not
# left a wiki-derived dataset in the tarball. See sidecar/data/ATTRIBUTION.md.
rm -f "${SPKG}/data/SOURCES.md"
if ! grep -q '"placeholder": true' "${SPKG}/data/items.json"; then
  echo "!! sidecar/data/items.json is not the placeholder - refusing to ship a generated catalogue" >&2
  echo "   (run: git checkout -- games/dune/sidecar/data/items.json)" >&2
  exit 1
fi

sed -e "s|@VERSION@|${VERSION}|g" -e "s|@REVISION@|${DUNE_REVISION}|g" -e "s|@TARGET@|${DUNE_TARGET}|g" \
    "${SCRIPT_DIR}/templates/sidecar-README.release.txt" > "${SPKG}/README.release.txt"
dune_stamp "${SPKG}"
pkg_tar_gz "${STAGE}" TakaroDuneSidecar "${OUT_DIR}/${SIDECAR_ARTIFACT}"
dune_meta "${SIDECAR_ARTIFACT}"
echo "  -> ${OUT_DIR}/${SIDECAR_ARTIFACT}"

# ── the plugin: optional, and pinned to this exact server build ──────────────
PPKG="${STAGE}/TakaroDune"
mkdir -p "${PPKG}"
cp "${PROJECT_ROOT}/mod/dist/libtakaro-dune.so" "${PPKG}/"
sed -e "s|@VERSION@|${VERSION}|g" -e "s|@REVISION@|${DUNE_REVISION}|g" -e "s|@TARGET@|${DUNE_TARGET}|g" \
    "${SCRIPT_DIR}/templates/plugin-README.txt" > "${PPKG}/README.txt"
dune_stamp "${PPKG}"
pkg_tar_gz "${STAGE}" TakaroDune "${OUT_DIR}/${PLUGIN_ARTIFACT}"
dune_meta "${PLUGIN_ARTIFACT}"
echo "  -> ${OUT_DIR}/${PLUGIN_ARTIFACT}"

pkg_sha256sums "${OUT_DIR}"
echo "==> ${OUT_DIR}/SHA256SUMS"
cat "${OUT_DIR}/SHA256SUMS"
