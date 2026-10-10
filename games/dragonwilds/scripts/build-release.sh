#!/usr/bin/env bash
# Builds the Takaro RuneScape: Dragonwilds connector for one catalog target and packages it
# deterministically as <out-dir>/<catalog artifact name> plus a .meta.json, and
# <out-dir>/SHA256SUMS:
#
#   TakaroDragonwilds/
#     libtakaro-dragonwilds.so        the plugin, which also holds the Takaro connection
#     takaro.cfg                      the connector settings, copied next to the plugin
#     env.example                     Docker .env template (game settings; overrides takaro.cfg)
#     docker-compose.example.yml      the game service with the plugin preloaded
#     README.txt, INSTALL.md          install, upgrade from the 0.2.x sidecar, rollback
#     scripts/drain-legacy.py         the one-time 0.2.x sidecar drain and state import
#     THIRD-PARTY.md, licenses/       what is linked into the plugin, and its license texts
#     takaro-target.json              the catalog target this build is pinned to
#     SHA256SUMS                      every file above
#
# Usage: build-release.sh <version> <out-dir> [--target <catalog target id>]
#
# The host needs no C++ toolchain: the plugin is built and tested inside an image made from
# the catalog's pinned Bookworm toolchain and its pinned, hash-checked native sources.
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
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${REPO_ROOT}" log -1 --format=%ct)}"
export TZ=UTC
SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"

for required in "${PROJECT_ROOT}/mod/third_party/README.md" "${PROJECT_ROOT}/mod/third_party/licenses"; do
    [ -e "${required}" ] || { echo "build-release: missing ${required}; the release must carry its licenses" >&2; exit 1; }
done

echo "Building the Dragonwilds connector ${VERSION} for ${DRAGONWILDS_TARGET} (${DRAGONWILDS_FP16})..."
dragonwilds_builder_image || { echo "the pinned builder image did not build" >&2; exit 6; }

# Every dependency the resolved target declares, handed over as environment: the source check
# inside the container compares the developer Dockerfile against exactly this set.
DEP_ARGS=()
while IFS='=' read -r key _; do
    case "$key" in
        DRAGONWILDS_DEP_*) DEP_ARGS+=(-e "$key") ;;
    esac
done < <(env)
[ ${#DEP_ARGS[@]} -gt 0 ] || { echo "the resolved target declares no build dependencies" >&2; exit 2; }

# Built and tested in the builder image, mounted at its own path so every path inside the
# container is the path outside it. A release starts from no earlier object file.
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e SOURCE_DATE_EPOCH \
    -e TZ \
    "${DEP_ARGS[@]}" \
    -v "${REPO_ROOT}:${REPO_ROOT}" \
    -w "${PROJECT_ROOT}" \
    "${BUILDER_IMAGE}" \
    bash -euo pipefail -c '
        node scripts/check-exact-source.mjs mod/Dockerfile.build
        rm -rf mod/build mod/dist mod/tests/build
        cd mod
        ./build.sh --native --tests'

SO="${PROJECT_ROOT}/mod/dist/libtakaro-dragonwilds.so"
[ -f "${SO}" ] || { echo "build-release: expected ${SO} after the build" >&2; exit 1; }

# shellcheck source=../../../scripts/lib/package.sh
. "${REPO_ROOT}/scripts/lib/package.sh"

STAGE="${PROJECT_ROOT}/_data/build/stage-${DRAGONWILDS_FP16}"
rm -rf "${STAGE}"
PKG="${STAGE}/TakaroDragonwilds"
mkdir -p "${PKG}/licenses" "${PKG}/scripts"

cp "${SO}" "${PKG}/"
cp "${PROJECT_ROOT}/takaro.cfg" "${PKG}/takaro.cfg"
# A dotfile is not a legal artifact archive entry (`paths.safe_relative` refuses a segment
# that does not start with an alphanumeric), so the example env ships as env.example.
cp "${PROJECT_ROOT}/.env.example" "${PKG}/env.example"
cp "${PROJECT_ROOT}/docker-compose.example.yml" "${PKG}/docker-compose.example.yml"
cp "${PROJECT_ROOT}/INSTALL.md" "${PKG}/INSTALL.md"
cp "${PROJECT_ROOT}/scripts/drain-legacy.py" "${PKG}/scripts/drain-legacy.py"
cp "${PROJECT_ROOT}/mod/third_party/README.md" "${PKG}/THIRD-PARTY.md"
cp "${PROJECT_ROOT}"/mod/third_party/licenses/* "${PKG}/licenses/"

cat > "${PKG}/README.txt" <<TXT
Takaro RuneScape: Dragonwilds connector ${VERSION}

Built for Steam build ${DRAGONWILDS_REVISION} (see takaro-target.json).

Server-side only. Players install nothing.
libtakaro-dragonwilds.so is loaded into the Linux dedicated server with LD_PRELOAD and
connects to Takaro itself; there is no sidecar. Preload it on the game binary's own launch
line only, never onto SteamCMD (32-bit; it fails with a 64-bit preload).

Copy takaro.cfg next to libtakaro-dragonwilds.so and paste your Takaro registration
token into it; saving the file is enough, also while the server runs.

Install, upgrade from the 0.2.x sidecar, and rollback: INSTALL.md.
Check the files you unpacked: sha256sum -c SHA256SUMS

Never commit or share a filled-in takaro.cfg or .env: they hold your registration token.
TXT

cat > "${PKG}/takaro-target.json" <<JSON
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

# Every file in the folder, by path, so an operator can check what they unpacked. The listing
# is written outside the folder first, so the sums never list their own file.
( cd "${PKG}" && find . -type f | sed 's|^\./||' | LC_ALL=C sort | xargs -r sha256sum ) > "${STAGE}/SHA256SUMS"
mv "${STAGE}/SHA256SUMS" "${PKG}/SHA256SUMS"

pkg_tar_gz "${STAGE}" TakaroDragonwilds "${OUT_DIR}/${PLUGIN_ARTIFACT}"

# A previous release run may have populated this output directory. Never leave a stale
# sidecar archive from an older build beside the one-component release.
rm -f "${OUT_DIR}"/takaro-dragonwilds-sidecar-*.tar.gz "${OUT_DIR}"/takaro-dragonwilds-sidecar-*.tar.gz.meta.json

# The identity beside the archive, because a tarball has no manifest to stamp.
# `takaro-maint artifact validate` reads this file.
cat > "${OUT_DIR}/${PLUGIN_ARTIFACT}.meta.json" <<JSON
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
echo "  -> ${OUT_DIR}/${PLUGIN_ARTIFACT}"

pkg_sha256sums "${OUT_DIR}"
echo "==> ${OUT_DIR}/SHA256SUMS"
cat "${OUT_DIR}/SHA256SUMS"
