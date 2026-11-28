#!/usr/bin/env bash
# Builds the Takaro DayZ release artifacts for one catalog target and packages them
# deterministically as <out-dir>/<catalog artifact name>:
#
#   takaro-dayz-mod-<target>-<version>.zip         @TakaroIntegration server mod (REQUIRED)
#   takaro-dayz-sidecar-<target>-<version>.tar.gz  the Node.js sidecar (REQUIRED)
#   SHA256SUMS                                     checksums of everything above
#
# Usage: build-release.sh <version> <out-dir> [--target <catalog target id>]
#
# The host needs only Docker: the script resolves the target, then runs itself again inside
# the image the catalog pins as this target's toolchain, with the resolved DAYZ_* keys handed
# over as environment. Everything is built from a staged copy, never from the source tree:
#   - the sidecar's lockfile is checked against the catalog's dependency pins, then
#     `npm ci` and `npm test` (tsc + node:test) run on the copy and produce dist/;
#   - the mod's Enforce Script is packed into TakaroIntegration.pbo by mod/tools/pack_pbo.py
#     with one fixed entry timestamp (there is no compiler step: the server compiles the
#     scripts when it loads the PBO).
# The release version is stamped into the staged mod (CfgMods version and the bridge's
# VERSION constant) and into the staged sidecar's package.json, so the source tree keeps
# its own values. Timestamps come from SOURCE_DATE_EPOCH (default: the last commit), so the
# same commit always packs to the same bytes.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
REPO_ROOT=$(cd -- "${PROJECT_ROOT}/../.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

USAGE="usage: build-release.sh <version> <out-dir> [--target <id>]"
VERSION="${1:?$USAGE}"
OUT_DIR="${2:?$USAGE}"
shift 2
dayz_parse_target_flag "$@"

# The version reaches file names, JSON documents and Enforce Script source, so it is checked
# once here rather than escaped everywhere.
case "${VERSION}" in
  *[!A-Za-z0-9._+-]*|"")
    echo "refusing version '${VERSION}': use letters, digits and . _ + - only" >&2
    exit 2
    ;;
esac

mkdir -p "${OUT_DIR}"
OUT_DIR=$(cd -- "${OUT_DIR}" && pwd)

# ── on the host: resolve, then run again inside the pinned toolchain image ──────────────
if [ "${DAYZ_IN_TOOLCHAIN:-}" != "1" ]; then
    dayz_resolve_target "${TARGET}"
    SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${REPO_ROOT}" log -1 --format=%ct)}"
    TAKARO_SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"
    export SOURCE_DATE_EPOCH TAKARO_SOURCE_REVISION
    ENV_ARGS=(-e SOURCE_DATE_EPOCH -e TAKARO_SOURCE_REVISION -e DAYZ_IN_TOOLCHAIN=1 -e TZ=UTC
        -e HOME=/tmp -e npm_config_cache=/tmp/npm-cache)
    while IFS='=' read -r key _; do
        case "$key" in
            DAYZ_*) ENV_ARGS+=(-e "$key") ;;
        esac
    done < <(env)
    # Mounted at their own paths, so every path inside the container is the path outside it.
    exec docker run --rm \
        --user "$(id -u):$(id -g)" \
        "${ENV_ARGS[@]}" \
        -v "${REPO_ROOT}:${REPO_ROOT}" \
        -v "${OUT_DIR}:${OUT_DIR}" \
        -w "${REPO_ROOT}" \
        "${DAYZ_TOOLCHAIN}" \
        bash "${SCRIPT_DIR}/build-release.sh" "${VERSION}" "${OUT_DIR}" --target "${DAYZ_TARGET}"
fi

# ── inside the toolchain image ────────────────────────────────────────────────────────
dayz_resolve_target "${TARGET}"
: "${SOURCE_DATE_EPOCH:?}" "${TAKARO_SOURCE_REVISION:?}"
MOD_ARTIFACT="${DAYZ_ARTIFACT_MOD/\{version\}/${VERSION}}"
SIDECAR_ARTIFACT="${DAYZ_ARTIFACT_SIDECAR/\{version\}/${VERSION}}"

# Every dependency the resolved target declares is checked against the lockfile; a target
# that declares none would make that check pass vacuously.
env | grep -q '^DAYZ_DEP_.*_URL=' || { echo "the resolved target declares no build dependencies" >&2; exit 2; }

# shellcheck source=../../../scripts/lib/package.sh
. "${REPO_ROOT}/scripts/lib/package.sh"

STAGE="${PROJECT_ROOT}/_data/build/stage-${DAYZ_FP16}"
rm -rf "${STAGE}"
mkdir -p "${STAGE}"

# dayz_stamp <folder> -- the identity the installed component carries and `deploy` reads.
dayz_stamp() {
    cat > "${1}/takaro-target.json" <<JSON
{
  "target": "${DAYZ_TARGET}",
  "fingerprint": "${DAYZ_FINGERPRINT}",
  "game": "dayz",
  "platform": "${DAYZ_PLATFORM}",
  "revision": "${DAYZ_REVISION}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${TAKARO_SOURCE_REVISION}"
}
JSON
}

# dayz_meta <artifact> -- the identity beside the archive. `takaro-maint artifact validate`
# reads this file for anything that is not a jar.
dayz_meta() {
    cat > "${OUT_DIR}/${1}.meta.json" <<JSON
{
  "target": "${DAYZ_TARGET}",
  "fingerprint": "${DAYZ_FINGERPRINT}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${TAKARO_SOURCE_REVISION}",
  "game": "dayz",
  "platform": "${DAYZ_PLATFORM}",
  "revision": "${DAYZ_REVISION}"
}
JSON
}

# dayz_replace <file> <sed expression> <what> -- a stamp that silently matched nothing would
# ship the source tree's version under the release's name, so a miss fails the build.
dayz_replace() {
    local file="$1" expression="$2" what="$3"
    sed -i -E "${expression}" "${file}"
    grep -qF "\"${VERSION}\"" "${file}" || { echo "build-release: could not stamp ${what} in ${file}" >&2; exit 1; }
}

# ── the sidecar ───────────────────────────────────────────────────────────────────────
SRC="${STAGE}/sidecar-src"
mkdir -p "${SRC}"
tar -C "${PROJECT_ROOT}/sidecar" \
    --exclude=./node_modules --exclude=./dist --exclude=./state --exclude='*.log' --exclude=./TakaroConfig.txt \
    -cf - . | tar -C "${SRC}" -xf -
cp "${SCRIPT_DIR}/check-exact-source.mjs" "${STAGE}/"
(
    cd "${SRC}"
    # Before anything is installed: `npm ci` would otherwise install whatever the lockfile
    # points at, recorded or not.
    node ../check-exact-source.mjs
    DAYZ_VERSION="${VERSION}" node -e '
        const fs = require("node:fs");
        const version = process.env.DAYZ_VERSION;
        const pkg = JSON.parse(fs.readFileSync("package.json", "utf8"));
        pkg.version = version;
        fs.writeFileSync("package.json", JSON.stringify(pkg, null, 2) + "\n");
        const lock = JSON.parse(fs.readFileSync("package-lock.json", "utf8"));
        lock.version = version;
        if (lock.packages && lock.packages[""]) lock.packages[""].version = version;
        fs.writeFileSync("package-lock.json", JSON.stringify(lock, null, 2) + "\n");'
    npm ci --no-audit --no-fund
    npm test
)
[ -f "${SRC}/dist/index.js" ] || { echo "build-release: the sidecar build produced no dist/index.js" >&2; exit 1; }

SPKG="${STAGE}/TakaroDayZSidecar"
mkdir -p "${SPKG}"
cp -R "${SRC}/dist" "${SPKG}/"
# The sidecar reads its game icon from assets/ beside dist/ (the game profile it sends Takaro).
mkdir -p "${SPKG}/assets"
cp "${PROJECT_ROOT}/assets/icon-256.png" "${SPKG}/assets/"
cp "${SRC}/package.json" "${SRC}/package-lock.json" "${SRC}/Dockerfile" \
   "${SRC}/TakaroConfig.example.txt" "${PROJECT_ROOT}/docker-compose.example.yml" \
   "${PROJECT_ROOT}/LICENSE" "${SPKG}/"
# Not the source tree's .dockerignore: that one excludes dist/ for the dev image, which
# compiles it, while the release Dockerfile copies the prebuilt dist/.
cp "${SCRIPT_DIR}/templates/sidecar.dockerignore.release" "${SPKG}/.dockerignore"
if grep -qx 'dist/\{0,1\}' "${SPKG}/.dockerignore"; then
    echo "build-release: the sidecar package's .dockerignore excludes dist/, which its Dockerfile copies" >&2
    exit 1
fi
sed -e "s|@VERSION@|${VERSION}|g" -e "s|@REVISION@|${DAYZ_REVISION}|g" -e "s|@TARGET@|${DAYZ_TARGET}|g" \
    "${SCRIPT_DIR}/templates/sidecar-README.release.txt" > "${SPKG}/README.txt"
dayz_stamp "${SPKG}"
pkg_tar_gz "${STAGE}" TakaroDayZSidecar "${OUT_DIR}/${SIDECAR_ARTIFACT}"
dayz_meta "${SIDECAR_ARTIFACT}"
echo "  -> ${OUT_DIR}/${SIDECAR_ARTIFACT}"

# ── the mod ───────────────────────────────────────────────────────────────────────────
MSRC="${STAGE}/mod-src"
cp -R "${PROJECT_ROOT}/mod/takaro_integration" "${MSRC}"
dayz_replace "${MSRC}/config.cpp" "s/^([[:space:]]*version[[:space:]]*=[[:space:]]*)\"[^\"]*\";/\\1\"${VERSION}\";/" \
    "the CfgMods version"
dayz_replace "${MSRC}/scripts/4_World/TakaroBridge.c" \
    "s/(static const string VERSION[[:space:]]*=[[:space:]]*)\"[^\"]*\";/\\1\"${VERSION}\";/" "the bridge VERSION"

MPKG="${STAGE}/@TakaroIntegration"
mkdir -p "${MPKG}/Addons"
python3 -I "${PROJECT_ROOT}/mod/tools/pack_pbo.py" "${MSRC}" "${MPKG}/Addons/TakaroIntegration.pbo" \
    --prefix TakaroIntegration --timestamp "${SOURCE_DATE_EPOCH}" >/dev/null
cp "${PROJECT_ROOT}/mod/@TakaroIntegration/mod.cpp" "${PROJECT_ROOT}/mod/@TakaroIntegration/meta.cpp" \
   "${PROJECT_ROOT}/LICENSE" "${MPKG}/"
dayz_stamp "${MPKG}"
pkg_normalize "${MPKG}"
# zip is not in the toolchain image; Python's zipfile is, with fixed entry times and order.
python3 -I - "${STAGE}" "@TakaroIntegration" "${OUT_DIR}/${MOD_ARTIFACT}" <<'PY'
import os, sys, time, zipfile
stage, folder, dest = sys.argv[1:4]
stamp = time.gmtime(max(int(os.environ["SOURCE_DATE_EPOCH"]), 315532800))[:6]
names = []
for root, _dirs, files in os.walk(os.path.join(stage, folder)):
    for name in files:
        names.append(os.path.relpath(os.path.join(root, name), stage).replace(os.sep, "/"))
tmp = dest + ".part"
with zipfile.ZipFile(tmp, "w") as archive:
    for name in sorted(names):
        info = zipfile.ZipInfo(name, stamp)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        info.create_system = 3
        with open(os.path.join(stage, name), "rb") as handle:
            archive.writestr(info, handle.read())
os.replace(tmp, dest)
print(f"{os.path.basename(dest)}: {len(names)} entries")
PY
dayz_meta "${MOD_ARTIFACT}"
echo "  -> ${OUT_DIR}/${MOD_ARTIFACT}"

pkg_sha256sums "${OUT_DIR}"
echo "==> ${OUT_DIR}/SHA256SUMS"
cat "${OUT_DIR}/SHA256SUMS"
