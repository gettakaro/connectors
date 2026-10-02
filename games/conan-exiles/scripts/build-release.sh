#!/usr/bin/env bash
# Builds the Conan Exiles TypeScript sidecar for one catalog target and packages it
# deterministically as <out-dir>/<catalog artifact name>.
#
# Usage: build-release.sh <version> <out-dir> [--target <catalog target id>]
#
# The host needs no Node: the build runs inside the image the catalog pins as this
# target's toolchain, which is the same image the server itself runs in.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
REPO_ROOT=$(cd -- "${PROJECT_ROOT}/../.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

VERSION="${1:?usage: build-release.sh <version> <out-dir> [--target <id>]}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir> [--target <id>]}"
shift 2
conan_parse_target_flag "$@"
conan_resolve_target "${TARGET}"

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
ARTIFACT="${CONAN_EXILES_ARTIFACT/\{version\}/${VERSION}}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${REPO_ROOT}" log -1 --format=%ct)}"
# CPython's ZipInfo takes local time, so the archive's entry timestamps depend on the
# builder's zone unless it is pinned here.
export TZ=UTC

# Every dependency the resolved target declares, discovered rather than listed here: a
# dependency added to the catalog must not be able to slip past the check below because
# somebody forgot to add a line to this script.
DEP_ARGS=()
while IFS='=' read -r key _; do
    case "$key" in
        CONAN_EXILES_DEP_*) DEP_ARGS+=(-e "$key") ;;
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
    -e CONAN_EXILES_LOCKFILE_PATH \
    -e CONAN_EXILES_LOCKFILE_SHA256 \
    "${DEP_ARGS[@]}" \
    -v "${REPO_ROOT}:${REPO_ROOT}" \
    -w "${REPO_ROOT}/games/conan-exiles/bridge" \
    "${CONAN_EXILES_TOOLCHAIN}" \
    sh -c 'set -eu
        node ../scripts/check-exact-source.mjs
        rm -rf dist node_modules
        npm ci --no-audit --no-fund
        npm test
        npm run build'

# The in-game chat library (LD_PRELOAD into the server), built in its own pinned toolchain
# (native/Dockerfile.build) with its tests. It guards on this target's server build-id itself.
"${PROJECT_ROOT}/native/build.sh" --tests

STAGE="${PROJECT_ROOT}/_data/build/stage-${CONAN_EXILES_FP16}"
PACKAGE_DIR="${STAGE}/TakaroConanExiles"
rm -rf "${STAGE}"
mkdir -p "${PACKAGE_DIR}"
cp -R "${PROJECT_ROOT}/bridge/dist" "${PACKAGE_DIR}/dist"
cp -R "${PROJECT_ROOT}/bridge/scripts" "${PACKAGE_DIR}/scripts"
cp "${PROJECT_ROOT}/bridge/package.json" "${PROJECT_ROOT}/bridge/package-lock.json" "${PACKAGE_DIR}/"
cp "${PROJECT_ROOT}/README.md" "${PROJECT_ROOT}/TakaroConfig.example.txt" "${PACKAGE_DIR}/"
rm -rf "${PACKAGE_DIR}/dist/__tests__"
mkdir -p "${PACKAGE_DIR}/native"
cp "${PROJECT_ROOT}/native/dist/libtakaro-conan-native.so" "${PROJECT_ROOT}/native/dist/SHA256SUMS" \
   "${PACKAGE_DIR}/native/"

# The release must be runnable with `npm ci --omit=dev`, so every entrypoint a
# package.json script points at has to exist in the packaged dist/ -- the bridge and the
# chat helper both, because they are one artifact and two processes.
for required in dist/index.js dist/mod/pollerCli.js native/libtakaro-conan-native.so; do
  if [ ! -f "${PACKAGE_DIR}/${required}" ]; then
    echo "build-release: missing ${required} in release package" >&2
    exit 1
  fi
done

# The identity the running bridge logs and serves on /health, and the one `deploy` reads.
cat > "${PACKAGE_DIR}/takaro-target.json" <<JSON
{
  "target": "${CONAN_EXILES_TARGET}",
  "fingerprint": "${CONAN_EXILES_FINGERPRINT}",
  "game": "conan-exiles",
  "platform": "linux",
  "revision": "${CONAN_EXILES_REVISION}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"
}
JSON

cat > "${PACKAGE_DIR}/README.release.txt" <<EOF
Takaro Conan Exiles Connector ${VERSION}

Built for Conan Exiles Dedicated Server build ${CONAN_EXILES_REVISION} (see takaro-target.json).

Install:
1. Extract this folder on the Conan Exiles dedicated server host.
2. Run npm ci --omit=dev.
3. Copy TakaroConfig.example.txt to TakaroConfig.txt.
4. Configure Takaro registration and Conan RCON values.
5. Start with npm start, on the same host (or in the same container network
   namespace) as the Conan server: the bridge's chat API listens on 127.0.0.1.
6. For in-game chat (sendMessage, Discord to game), start the Conan server with
   the native library preloaded; no client or server mod is needed:
     LD_PRELOAD=/path/to/TakaroConanExiles/native/libtakaro-conan-native.so ./ConanSandboxServer.sh ...
   It only hooks build ${CONAN_EXILES_REVISION}; on any other build it does nothing
   and logs why in ConanSandbox/Saved/Logs/TakaroConanNative.log.
   TAKARO_CONAN_BRIDGE_URL (default http://127.0.0.1:3010) points it at the bridge.

npm start runs from dist/ and needs only production dependencies.

Do not commit live registration tokens or RCON passwords.
EOF

# Neither the host nor the pinned Node image has `zip`, so the archive is written by
# CPython: `zipfile -c` walks sorted(os.listdir) and deflates each file with the mode and
# mtime it finds, which `pkg_normalize` has just made the same everywhere. With TZ=UTC
# pinned above, two builds of one commit produce identical bytes.
# shellcheck source=../../../scripts/lib/package.sh
. "${REPO_ROOT}/scripts/lib/package.sh"
pkg_normalize "${PACKAGE_DIR}"
rm -f "${OUT_DIR}/${ARTIFACT}"
( cd "${STAGE}" && UV_PYTHON_PREFERENCE=only-managed uv run --frozen --project "${REPO_ROOT}/maintenance" \
    python -m zipfile -c "${OUT_DIR}/${ARTIFACT}" TakaroConanExiles )

# The identity the artifact carries beside it: `takaro-maint artifact validate` reads this
# file, because a zip has no manifest to stamp.
cat > "${OUT_DIR}/${ARTIFACT}.meta.json" <<JSON
{
  "target": "${CONAN_EXILES_TARGET}",
  "fingerprint": "${CONAN_EXILES_FINGERPRINT}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}",
  "game": "conan-exiles",
  "platform": "linux",
  "revision": "${CONAN_EXILES_REVISION}"
}
JSON

echo "  -> ${OUT_DIR}/${ARTIFACT}"
sha256sum "${OUT_DIR}/${ARTIFACT}"
