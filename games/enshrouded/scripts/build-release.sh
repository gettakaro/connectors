#!/usr/bin/env bash
# Builds the Enshrouded connector for one catalog target and packages it deterministically as
# <out-dir>/<catalog artifact name> plus a .meta.json, and <out-dir>/SHA256SUMS.
#
#   TakaroEnshrouded/                 copy its contents next to enshrouded_server.exe
#     dbghelp.dll                     the plugin, which also holds the Takaro connection
#     takaro/plugin.json.example      the configuration, to be renamed plugin.json
#     README.txt, INSTALL.md          install, upgrade and rollback
#     THIRD-PARTY.md, licenses/       what is linked into the DLL, and its license texts
#     SHA256SUMS                      every file above
#
# Usage: build-release.sh <version> <out-dir> --target <catalog target id>
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
REPO_ROOT=$(cd -- "${PROJECT_ROOT}/../.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

VERSION="${1:?usage: build-release.sh <version> <out-dir> --target <id>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir> --target <id>}"
shift 2
enshrouded_parse_target_flag "$@"
[ -n "${TARGET}" ] || { echo "Enshrouded is built per catalog target: pass --target <id>" >&2; exit 2; }
enshrouded_resolve_target "${TARGET}"

# The version reaches a JSON document, a file name and a command inside the builder
# container, so it is checked once here rather than escaped three times.
case "${VERSION}" in
  *[!A-Za-z0-9._+-]*|"")
    echo "refusing version '${VERSION}': use letters, digits and . _ + - only" >&2
    exit 2
    ;;
esac

mkdir -p "${OUT_DIR}"
OUT_DIR=$(cd -- "${OUT_DIR}" && pwd)
PLUGIN_ZIP="${ENSHROUDED_ARTIFACT_SERVER_PLUGIN/\{version\}/${VERSION}}"
export PLUGIN_ZIP VERSION
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${REPO_ROOT}" log -1 --format=%ct)}"

# A release is built from nothing but the sources and the pinned inputs, so no object file
# from an earlier build of another target or version can reach the archive.
rm -rf "${PROJECT_ROOT}/mod/build" "${PROJECT_ROOT}/mod/build-debug" "${PROJECT_ROOT}/_data/build"

echo "Building the Enshrouded connector ${VERSION} for ${ENSHROUDED_TARGET} (${ENSHROUDED_FP16})..."
enshrouded_builder_image || { echo "the pinned builder image did not build" >&2; exit 6; }

# Everything is compiled and packaged inside the toolchain image: the host has neither zig
# nor zip, and the archive has to be byte-identical wherever it is built. The names are
# handed over as environment entries and quoted inside the container, so the program the
# container runs is the same one whatever the version string contains.
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e ZIG=/opt/zig/zig \
    -e SOURCE_DATE_EPOCH \
    -e VERSION \
    -e PLUGIN_ZIP \
    -v "${REPO_ROOT}:/repo" \
    -v "${OUT_DIR}:/out" \
    -w /repo/games/enshrouded \
    "${BUILDER_IMAGE}" bash -euo pipefail -c '
      ./mod/build.sh

      STAGE=_data/build/stage
      PKG="$STAGE/TakaroEnshrouded"
      rm -rf "$STAGE"
      mkdir -p "$PKG/takaro" "$PKG/licenses"

      cp mod/build/dbghelp.dll "$PKG/"
      cp scripts/templates/plugin.json.example "$PKG/takaro/plugin.json.example"
      sed "s/@VERSION@/${VERSION}/g" scripts/templates/plugin-README.txt > "$PKG/README.txt"
      cp INSTALL.md "$PKG/INSTALL.md"
      cp mod/third_party/README.md "$PKG/THIRD-PARTY.md"
      # The license of every piece the DLL links: MinHook from this tree, and the parts of
      # the pinned zig toolchain that end up in a Windows DLL (compiler-rt, libc++, libc++abi,
      # libunwind, the mingw-w64 runtime).
      cp mod/third_party/minhook/LICENSE.txt "$PKG/licenses/MinHook-LICENSE.txt"
      cp /opt/zig/LICENSE "$PKG/licenses/zig-LICENSE.txt"
      cp /opt/zig/lib/libcxx/LICENSE.TXT "$PKG/licenses/libcxx-LICENSE.txt"
      cp /opt/zig/lib/libcxxabi/LICENSE.TXT "$PKG/licenses/libcxxabi-LICENSE.txt"
      cp /opt/zig/lib/libunwind/LICENSE.TXT "$PKG/licenses/libunwind-LICENSE.txt"
      cp /opt/zig/lib/libc/mingw/COPYING "$PKG/licenses/mingw-w64-COPYING.txt"
      # Every file in the folder, by path, so an operator can check what they unpacked. The
      # listing is written outside the folder first, so the sums never list their own file.
      ( cd "$PKG" && find . -type f | sed "s|^\./||" | LC_ALL=C sort | xargs -r sha256sum ) > "$STAGE/SHA256SUMS"
      mv "$STAGE/SHA256SUMS" "$PKG/SHA256SUMS"

      . /repo/scripts/lib/package.sh
      pkg_zip "$STAGE" TakaroEnshrouded "/out/${PLUGIN_ZIP}"
    '

# A previous release run may have populated this output directory. Never leave a stale
# sidecar archive from an older build beside the one-component release.
rm -f "${OUT_DIR}"/takaro-enshrouded-sidecar-*.zip "${OUT_DIR}"/takaro-enshrouded-sidecar-*.zip.meta.json

# The identity the artifact carries: `takaro-maint artifact validate` reads this file,
# because a zip has no manifest to stamp.
SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"
cat > "${OUT_DIR}/${PLUGIN_ZIP}.meta.json" <<JSON
{
  "target": "${ENSHROUDED_TARGET}",
  "fingerprint": "${ENSHROUDED_FINGERPRINT}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${SOURCE_REVISION}",
  "game": "enshrouded",
  "platform": "proton",
  "revision": "${ENSHROUDED_REVISION}"
}
JSON
( cd "${OUT_DIR}" && sha256sum "${PLUGIN_ZIP}" > SHA256SUMS )
echo "  -> ${OUT_DIR}/${PLUGIN_ZIP}"
cat "${OUT_DIR}/SHA256SUMS"
