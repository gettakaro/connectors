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
# .meta.json is written here; `takaro-maint build` writes one from the manifest row.
echo "  -> ${OUT_DIR}/${ARTIFACT}"
sha256sum "${OUT_DIR}/${ARTIFACT}"
