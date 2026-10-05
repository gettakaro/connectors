#!/usr/bin/env bash
# Builds the Conan Exiles connector for one catalog target and packages it deterministically as
# <out-dir>/<catalog artifact name> (+ .meta.json each) and <out-dir>/SHA256SUMS.
#
#   linux target    takaro-conan-exiles-native-linux-<build>-<version>.zip
#                     TakaroConanNative/  libtakaro-conan-native.so (LD_PRELOAD), ca-certificates.crt,
#                                         takaro.json.example, INSTALL.md, README.txt, licenses
#   windows target  takaro-conan-exiles-native-windows-<build>-<version>.zip
#                     TakaroConanNative/  winmm.dll (next to the server exe), takaro.json.example,
#                                         INSTALL.md, README.txt, licenses
#
# Usage: build-release.sh <version> <out-dir> [--target <catalog target id>]
#
# The host needs no compiler: every build runs inside a pinned image (the catalog toolchain for
# the Windows DLL, native/platform/linux/Dockerfile.build for the Linux library).
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
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${REPO_ROOT}" log -1 --format=%ct)}"
# CPython's ZipInfo takes local time, so the archive's entry timestamps depend on the
# builder's zone unless it is pinned here.
export TZ=UTC
SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"
PLATFORM="${CONAN_EXILES_PLATFORM:?the resolved target names no platform}"
STAGE_ROOT="${PROJECT_ROOT}/_data/build/stage-${CONAN_EXILES_FP16}"
rm -rf "${STAGE_ROOT}"
mkdir -p "${STAGE_ROOT}"
PRODUCED=()

# shellcheck source=../../../scripts/lib/package.sh
. "${REPO_ROOT}/scripts/lib/package.sh"

# zip_folder <stage> <folder> <artifact>: neither the host nor the pinned images have `zip`, so
# the archive is written by CPython: `zipfile -c` walks sorted(os.listdir) and deflates each
# file with the mode and mtime `pkg_normalize` has just made the same everywhere.
zip_folder() {
  local stage="$1" folder="$2" artifact="$3"
  pkg_normalize "${stage}/${folder}"
  rm -f "${OUT_DIR}/${artifact}"
  ( cd "${stage}" && UV_PYTHON_PREFERENCE=only-managed uv run --frozen --project "${REPO_ROOT}/maintenance" \
      python -m zipfile -c "${OUT_DIR}/${artifact}" "${folder}" )
  PRODUCED+=("${artifact}")
}

# write_meta <artifact>: the identity the artifact carries beside it; `takaro-maint artifact
# validate` reads this file, because a zip has no manifest to stamp.
write_meta() {
  cat > "${OUT_DIR}/$1.meta.json" <<JSON
{
  "target": "${CONAN_EXILES_TARGET}",
  "fingerprint": "${CONAN_EXILES_FINGERPRINT}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${SOURCE_REVISION}",
  "game": "conan-exiles",
  "platform": "${PLATFORM}",
  "revision": "${CONAN_EXILES_REVISION}"
}
JSON
}

# write_stamp <folder>: the same identity inside the package.
write_stamp() {
  cat > "$1/takaro-target.json" <<JSON
{
  "target": "${CONAN_EXILES_TARGET}",
  "fingerprint": "${CONAN_EXILES_FINGERPRINT}",
  "game": "conan-exiles",
  "platform": "${PLATFORM}",
  "revision": "${CONAN_EXILES_REVISION}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${SOURCE_REVISION}"
}
JSON
}

# native_common <folder> <binary>: everything both native packages carry besides the binary.
native_common() {
  local pkg="$1" binary="$2"
  mkdir -p "${pkg}/licenses"
  cp "${SCRIPT_DIR}/templates/takaro.${PLATFORM}.json.example" "${pkg}/takaro.json.example"
  cp "${PROJECT_ROOT}/INSTALL.md" "${pkg}/INSTALL.md"
  cp "${PROJECT_ROOT}/native/third_party/README.md" "${pkg}/THIRD-PARTY.md"
  sed -e "s/@VERSION@/${VERSION}/g" -e "s/@REVISION@/${CONAN_EXILES_REVISION}/g" \
      -e "s/@PLATFORM@/${PLATFORM}/g" -e "s/@BINARY@/${binary}/g" \
      "${SCRIPT_DIR}/templates/native-README.txt" > "${pkg}/README.txt"
  write_stamp "${pkg}"
}

# sums <folder>: every file in the folder, by path, so an operator can check what they unpacked.
# Written outside the folder first, so the sums never list their own file.
sums() {
  ( cd "$1" && find . -type f | sed 's|^\./||' | LC_ALL=C sort | xargs -r sha256sum ) > "$1.SHA256SUMS"
  mv "$1.SHA256SUMS" "$1/SHA256SUMS"
}

build_native_linux() {
  # Built and tested in its own pinned buster toolchain; it guards on this target's GNU build-id.
  "${PROJECT_ROOT}/native/build.sh" --tests
  local artifact="${CONAN_EXILES_ARTIFACT_NATIVE/\{version\}/${VERSION}}"
  local stage="${STAGE_ROOT}/native" pkg="${STAGE_ROOT}/native/TakaroConanNative"
  mkdir -p "${pkg}"
  cp "${PROJECT_ROOT}/native/dist/libtakaro-conan-native.so" "${pkg}/"
  # The library trusts only the CA file it is given; the bundle is the ca-certificates package
  # of the toolchain image, which comes from a dated Debian snapshot.
  docker run --rm takaro-conan-native-build:stage2 cat /etc/ssl/certs/ca-certificates.crt > "${pkg}/ca-certificates.crt"
  [ -s "${pkg}/ca-certificates.crt" ] || { echo "build-release: empty CA bundle" >&2; exit 1; }
  native_common "${pkg}" libtakaro-conan-native.so
  cp "${PROJECT_ROOT}/native/third_party/licenses/OpenSSL-LICENSE.txt" \
     "${PROJECT_ROOT}/native/third_party/licenses/libwebsockets-LICENSE" "${pkg}/licenses/"
  sums "${pkg}"
  zip_folder "${stage}" TakaroConanNative "${artifact}"
  write_meta "${artifact}"
}

build_native_windows() {
  local artifact="${CONAN_EXILES_ARTIFACT_NATIVE/\{version\}/${VERSION}}"
  local stage="${STAGE_ROOT}/native" pkg="${STAGE_ROOT}/native/TakaroConanNative"
  local image="takaro-conan-windows-builder:${CONAN_EXILES_FP16}"
  docker build -q \
      -f "${PROJECT_ROOT}/native/platform/windows/Dockerfile.builder" \
      --build-arg "TOOLCHAIN=${CONAN_EXILES_TOOLCHAIN:?}" \
      --build-arg "ZIG_URL=${CONAN_EXILES_DEP_ZIG_URL:?the windows target declares no zig}" \
      --build-arg "ZIG_SHA256=${CONAN_EXILES_DEP_ZIG_SHA256:?}" \
      -t "${image}" "${PROJECT_ROOT}/native/platform/windows" >/dev/null
  mkdir -p "${pkg}/licenses"
  # Cross-compiled in the pinned image; the build checks the vendored MinHook, the exports and
  # zeroes the link timestamps. The zig runtime's licenses come from the same image.
  docker run --rm \
      --user "$(id -u):$(id -g)" \
      -e HOME=/tmp \
      -v "${REPO_ROOT}:${REPO_ROOT}" \
      -w "${PROJECT_ROOT}/native" \
      -e PKG="${pkg}" \
      "${image}" bash -euo pipefail -c '
        platform/windows/build.sh
        cp dist-windows/winmm.dll "$PKG/winmm.dll"
        cp third_party/licenses/MinHook-LICENSE.txt "$PKG/licenses/MinHook-LICENSE.txt"
        cp /opt/zig/LICENSE "$PKG/licenses/zig-LICENSE.txt"
        cp /opt/zig/lib/libcxx/LICENSE.TXT "$PKG/licenses/libcxx-LICENSE.txt"
        cp /opt/zig/lib/libcxxabi/LICENSE.TXT "$PKG/licenses/libcxxabi-LICENSE.txt"
        cp /opt/zig/lib/libunwind/LICENSE.TXT "$PKG/licenses/libunwind-LICENSE.txt"
        cp /opt/zig/lib/libc/mingw/COPYING "$PKG/licenses/mingw-w64-COPYING.txt"
      '
  native_common "${pkg}" winmm.dll
  sums "${pkg}"
  zip_folder "${stage}" TakaroConanNative "${artifact}"
  write_meta "${artifact}"
}

case "${PLATFORM}" in
  linux)
    build_native_linux
    ;;
  windows)
    build_native_windows
    ;;
  *)
    echo "build-release: no build for platform '${PLATFORM}'" >&2
    exit 2
    ;;
esac

( cd "${OUT_DIR}" && sha256sum "${PRODUCED[@]}" > SHA256SUMS )
for artifact in "${PRODUCED[@]}"; do
  echo "  -> ${OUT_DIR}/${artifact}"
done
cat "${OUT_DIR}/SHA256SUMS"
