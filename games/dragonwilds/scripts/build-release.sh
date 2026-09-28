#!/usr/bin/env bash
# Builds the Takaro RuneScape: Dragonwilds release artifacts for one catalog target and
# packages them deterministically as <out-dir>/<catalog artifact name>:
#
#   takaro-dragonwilds-plugin-<target>-<version>.tar.gz    the LD_PRELOAD plugin
#   takaro-dragonwilds-sidecar-<target>-<version>.tar.gz   the sidecar (REQUIRED, the connector)
#   SHA256SUMS                                             checksums of everything above
#
# Usage: build-release.sh <version> <out-dir> [--target <catalog target id>]
#
# The host needs neither Node nor a C++ toolchain: both halves are built inside the image
# the catalog pins as this target's toolchain. node:*-bookworm (not slim) is that image
# precisely because it carries g++ on the same glibc as the dedicated server image -- a
# plugin linked against a newer glibc could not be preloaded into the game binary.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
REPO_ROOT=$(cd -- "${PROJECT_ROOT}/../.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

VERSION="${1:?usage: build-release.sh <version> <out-dir> [--target <id>]}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir> [--target <id>]}"
shift 2
dragonwilds_parse_target_flag "$@"
dragonwilds_resolve_target "${TARGET}"

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
PLUGIN_ARTIFACT="${DRAGONWILDS_ARTIFACT_PLUGIN/\{version\}/${VERSION}}"
SIDECAR_ARTIFACT="${DRAGONWILDS_ARTIFACT_SIDECAR/\{version\}/${VERSION}}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${REPO_ROOT}" log -1 --format=%ct)}"
export TZ=UTC
SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"

# Every dependency the resolved target declares, discovered rather than listed here: a
# dependency added to the catalog must not be able to slip past the check below because
# somebody forgot to add a line to this script.
DEP_ARGS=()
while IFS='=' read -r key _; do
    case "$key" in
        DRAGONWILDS_DEP_*) DEP_ARGS+=(-e "$key") ;;
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
    -w "${REPO_ROOT}/games/dragonwilds" \
    "${DRAGONWILDS_TOOLCHAIN}" \
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

STAGE="${PROJECT_ROOT}/_data/build/stage-${DRAGONWILDS_FP16}"
rm -rf "${STAGE}"
mkdir -p "${STAGE}"

# dragonwilds_stamp <folder> — the identity the running component logs and `deploy` reads.
dragonwilds_stamp() {
    cat > "${1}/takaro-target.json" <<JSON
{
  "target": "${DRAGONWILDS_TARGET}",
  "fingerprint": "${DRAGONWILDS_FINGERPRINT}",
  "game": "dragonwilds",
  "platform": "linux",
  "revision": "${DRAGONWILDS_REVISION}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${SOURCE_REVISION}"
}
JSON
}

# dragonwilds_meta <artifact> — the identity beside the archive, because a tarball has no
# manifest to stamp. `takaro-maint artifact validate` reads this file.
dragonwilds_meta() {
    cat > "${OUT_DIR}/${1}.meta.json" <<JSON
{
  "target": "${DRAGONWILDS_TARGET}",
  "fingerprint": "${DRAGONWILDS_FINGERPRINT}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${SOURCE_REVISION}",
  "game": "dragonwilds",
  "platform": "linux",
  "revision": "${DRAGONWILDS_REVISION}"
}
JSON
}

# ── the plugin: pinned to this exact server build ────────────────────────────
PPKG="${STAGE}/TakaroDragonwilds"
mkdir -p "${PPKG}"
cp "${PROJECT_ROOT}/mod/dist/libtakaro-dragonwilds.so" "${PPKG}/"
cat > "${PPKG}/README.txt" <<TXT
Takaro RuneScape: Dragonwilds plugin ${VERSION}

Built for build ${DRAGONWILDS_REVISION} (see takaro-target.json).

Server-side only. Players install nothing.
The plugin is loaded into the Linux dedicated server with LD_PRELOAD and serves a
loopback HTTP API on 127.0.0.1:18890 for the sidecar. See the connector README for the
full install steps; the plugin must be loaded onto the game binary's own launch line only,
never onto SteamCMD (32-bit; it fails with a 64-bit preload).

You also need takaro-dragonwilds-sidecar-*.tar.gz -- the plugin alone does not talk to
Takaro.
TXT
dragonwilds_stamp "${PPKG}"
pkg_tar_gz "${STAGE}" TakaroDragonwilds "${OUT_DIR}/${PLUGIN_ARTIFACT}"
dragonwilds_meta "${PLUGIN_ARTIFACT}"
echo "  -> ${OUT_DIR}/${PLUGIN_ARTIFACT}"

# ── the sidecar: the connector itself ─────────────────────────────────────────
SPKG="${STAGE}/TakaroDragonwildsSidecar"
mkdir -p "${SPKG}"
cp -R "${PROJECT_ROOT}/sidecar/dist" "${SPKG}/"
cp "${PROJECT_ROOT}/sidecar/package.json" "${PROJECT_ROOT}/sidecar/package-lock.json" \
   "${PROJECT_ROOT}/sidecar/Dockerfile" "${SPKG}/"
rm -rf "${SPKG}/dist/__tests__" "${SPKG}/dist/testing"
# A dotfile is not a legal artifact archive entry (`paths.safe_relative` refuses a segment
# that does not start with an alphanumeric, the same guard that keeps a component archive
# from ever writing outside its install directory), so the example env file the README
# tells an operator to copy ships as a non-dotfile name instead.
cp "${PROJECT_ROOT}/sidecar/.env.example" "${SPKG}/env.example"

# The release must be runnable with `npm ci --omit=dev`, so the entrypoint package.json
# points at has to exist in the packaged dist/.
[ -f "${SPKG}/dist/index.js" ] || { echo "build-release: missing dist/index.js in the sidecar package" >&2; exit 1; }

cat > "${SPKG}/README.release.txt" <<TXT
Takaro RuneScape: Dragonwilds sidecar ${VERSION}

Built for build ${DRAGONWILDS_REVISION} (see takaro-target.json). The sidecar reads the
plugin's loopback API (http://127.0.0.1:18890) and the server log, so it must share the
game server's network namespace (compose: network_mode: "service:dragonwilds") or run on
the same host. See the connector README for the full setup.

Never commit or share live registration tokens or plugin tokens.
TXT
dragonwilds_stamp "${SPKG}"
pkg_tar_gz "${STAGE}" TakaroDragonwildsSidecar "${OUT_DIR}/${SIDECAR_ARTIFACT}"
dragonwilds_meta "${SIDECAR_ARTIFACT}"
echo "  -> ${OUT_DIR}/${SIDECAR_ARTIFACT}"

pkg_sha256sums "${OUT_DIR}"
echo "==> ${OUT_DIR}/SHA256SUMS"
cat "${OUT_DIR}/SHA256SUMS"
