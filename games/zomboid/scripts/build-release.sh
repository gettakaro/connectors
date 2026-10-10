#!/usr/bin/env bash
# Builds the Project Zomboid connector agent for one catalog target and collects the
# shaded -javaagent jar into <out-dir> under the exact name the catalog gives it.
#
# Usage: build-release.sh <version> <out-dir> [--target <catalog target id>] [-- <gradle args>]
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
REPO_ROOT=$(cd -- "${PROJECT_ROOT}/../.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"
# shellcheck source=lib-gradle.sh
. "${SCRIPT_DIR}/lib-gradle.sh"

VERSION="${1:?usage: build-release.sh <version> <out-dir> [--target <id>]}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir> [--target <id>]}"
shift 2
zomboid_parse_target_flag "$@"
zomboid_resolve_target "${TARGET}"

# The version reaches a JSON document, a jar manifest and a command inside the toolchain
# container, so it is checked once here rather than escaped three times.
case "${VERSION}" in
  *[!A-Za-z0-9._+-]*|"")
    echo "refusing version '${VERSION}': use letters, digits and . _ + - only" >&2
    exit 2
    ;;
esac

mkdir -p "${OUT_DIR}"
OUT_DIR=$(cd -- "${OUT_DIR}" && pwd)
ARTIFACT="${ZOMBOID_ARTIFACT/\{version\}/${VERSION}}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${REPO_ROOT}" log -1 --format=%ct)}"
SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"

cd "${PROJECT_ROOT}"
# A release is built from nothing but the sources and the pinned inputs, so no earlier
# target's or version's class output can be carried into this jar.
rm -rf ./mod/agent/build ./mod/core/build

"${SCRIPT_DIR}/setup-environment.sh" --target "${ZOMBOID_TARGET}"

mapfile -t properties < <(zomboid_gradle_target_properties "${REPO_ROOT}" "${VERSION}")
echo "Building ${ARTIFACT} for ${ZOMBOID_TARGET} in ${ZOMBOID_TOOLCHAIN}..."
# shellcheck disable=SC2086 # EXTRA_GRADLE_ARGS is a deliberate word-split of caller flags
zomboid_gradle "${REPO_ROOT}" :agent:shadowJar \
    "-Pversion=${VERSION}" \
    "-PtakaroSourceRevision=${SOURCE_REVISION}" \
    "${properties[@]}" ${EXTRA_GRADLE_ARGS}

# The exact catalog-derived name, never a glob: a jar under any other name is a build that
# did not produce this target's artifact.
BUILT="./mod/agent/build/libs/${ARTIFACT}"
[ -f "${BUILT}" ] || { echo "error: the build produced no ${ARTIFACT}" >&2; exit 6; }
cp "${BUILT}" "${OUT_DIR}/${ARTIFACT}"

# A jar carries its identity in its own manifest and META-INF/takaro-target.json, so no
# .meta.json is written for it; `takaro-maint build` writes one from the manifest row.
echo "  -> ${OUT_DIR}/${ARTIFACT}"
sha256sum "${OUT_DIR}/${ARTIFACT}"

# The download for server owners: Takaro/TakaroConnector.jar and Takaro/TakaroConfig.txt,
# unpacked into the Zomboid data folder, so there is nothing to rename and no config file
# to write by hand. The config is the template the agent writes when the file is missing.
if [ -n "${ZOMBOID_ARTIFACT_BUNDLE:-}" ]; then
    BUNDLE="${ZOMBOID_ARTIFACT_BUNDLE/\{version\}/${VERSION}}"
    TEMPLATE="${PROJECT_ROOT}/mod/agent/src/main/resources/io/takaro/zomboid/agent/TakaroConfig.txt"
    # Python's zipfile rather than zip(1), which neither the host nor the JDK image has;
    # one timestamp, one mode and a fixed order keep two builds of a commit byte-identical.
    python3 -I - "${OUT_DIR}/${ARTIFACT}" "${TEMPLATE}" "${OUT_DIR}/${BUNDLE}" "${SOURCE_DATE_EPOCH}" <<'PY'
import os
import sys
import time
import zipfile

jar, template, out, epoch = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
stamp = time.gmtime(max(epoch, 315532800))[:6]
with zipfile.ZipFile(out + ".tmp", "w") as archive:
    for name, source in (("Takaro/TakaroConfig.txt", template), ("Takaro/TakaroConnector.jar", jar)):
        info = zipfile.ZipInfo(name, date_time=stamp)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.create_system = 3
        info.external_attr = 0o100644 << 16
        with open(source, "rb") as handle:
            archive.writestr(info, handle.read())
os.replace(out + ".tmp", out)
PY
    # A zip has no manifest to stamp; `takaro-maint artifact validate` reads this file.
    cat > "${OUT_DIR}/${BUNDLE}.meta.json" <<JSON
{
  "target": "${ZOMBOID_TARGET}",
  "fingerprint": "${ZOMBOID_FINGERPRINT}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${SOURCE_REVISION}",
  "game": "zomboid",
  "platform": "linux",
  "revision": "${ZOMBOID_REVISION}"
}
JSON
    echo "  -> ${OUT_DIR}/${BUNDLE}"
    sha256sum "${OUT_DIR}/${BUNDLE}"
fi
