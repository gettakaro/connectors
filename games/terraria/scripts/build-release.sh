#!/usr/bin/env bash
# Builds both Terraria connector roles for one catalog target and packages each one
# deterministically as <out-dir>/<catalog artifact name>, with its .meta.json beside it.
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
terraria_parse_target_flag "$@"
terraria_resolve_target "${TARGET}"

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
export PLUGIN_ARTIFACT="${TERRARIA_PLUGIN_ARTIFACT/\{version\}/${VERSION}}"
export BRIDGE_ARTIFACT="${TERRARIA_BRIDGE_ARTIFACT/\{version\}/${VERSION}}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${REPO_ROOT}" log -1 --format=%ct)}"
export VERSION

cd "${PROJECT_ROOT}"
echo "Building Terraria ${TERRARIA_TARGET} v${VERSION} (plugin + bridge)..."

"${SCRIPT_DIR}/setup-environment.sh" --target "${TERRARIA_TARGET}"
"${SCRIPT_DIR}/build-mod.sh" --target "${TERRARIA_TARGET}"
"${SCRIPT_DIR}/build-bridge.sh" --target "${TERRARIA_TARGET}"

STAGE="./_data/build/stage"
rm -rf "${STAGE}"
mkdir -p "${STAGE}/TakaroTerrariaEvents" "${STAGE}/TakaroTerrariaBridge"

cp ./_data/build/TakaroTerrariaEvents/TakaroTerrariaEvents.dll "${STAGE}/TakaroTerrariaEvents/"
cat > "${STAGE}/TakaroTerrariaEvents/README.txt" <<EOF
Takaro Terraria Events Plugin ${VERSION}

Built against ${TERRARIA_TARGET} (TShock ${TERRARIA_TSHOCK_TAG}).
Compile references: ${TERRARIA_IMAGE}

Install:
1. Copy TakaroTerrariaEvents.dll into the TShock server's additional-plugins directory.
2. Restart the server.
3. Confirm the TShock log contains "Takaro Terraria Events plugin loaded".
4. Grant the connector/admin user the "takaro.admin" TShock permission.

This is a server-side TShock plugin. It does not require a client mod, and it is not the
Takaro connector on its own: deploy takaro-terraria-bridge-${TERRARIA_TARGET}-${VERSION}.zip too.
EOF

cp -R ./bridge/dist ./bridge/node_modules ./bridge/package.json ./bridge/package-lock.json \
      "${STAGE}/TakaroTerrariaBridge/"
# The real config, not an example to rename: people paste the token into it and are done.
cp ./bridge/TakaroConfig.example.txt "${STAGE}/TakaroTerrariaBridge/TakaroConfig.txt"
cp ./README.md "${STAGE}/TakaroTerrariaBridge/"
rm -rf "${STAGE}/TakaroTerrariaBridge/dist/__tests__"
# Every path segment a deployed archive may carry has to start with an alphanumeric, and npm
# leaves metadata that does not (node_modules/.package-lock.json, node_modules/.bin shims).
# Running dist/index.js needs none of it, so it does not travel.
find "${STAGE}/TakaroTerrariaBridge/node_modules" -name '.*' -prune -exec rm -rf {} +
cat > "${STAGE}/TakaroTerrariaBridge/README.release.txt" <<EOF
Takaro Terraria Bridge ${VERSION}

Built for ${TERRARIA_TARGET} (TShock ${TERRARIA_TSHOCK_TAG}) on ${TERRARIA_BRIDGE_IMAGE}.

Install:
1. Extract this folder on the TShock server host.
2. Enable RestApiEnabled in tshock/config.json and set an application REST token.
3. In TakaroConfig.txt, paste the Takaro registration token into registrationToken= and the
   TShock REST token into tshockToken=. Leave identityToken empty; the bridge fills it in.
4. Start with: node dist/index.js

The bridge reads TakaroConfig.txt every few seconds, so a token saved while it runs is picked
up without a restart. It keeps a copy of its settings next to this folder
(TakaroTerrariaBridge.saved-config.txt), so replacing this folder on an upgrade keeps them.

node_modules is already in this archive (npm ci --omit=dev, lock-pinned), so there is
nothing to install. Run npm ci --omit=dev only if you delete it.

Do not commit live registration tokens or TShock REST tokens.
EOF

# Packaged inside the toolchain image: the host has no zip, and the archives have to be
# byte-identical wherever they are built. The names are handed over as environment entries
# and quoted inside the container, so the program it runs is the same whatever they hold.
docker compose run --rm --build \
    --user "$(id -u):$(id -g)" \
    -e SOURCE_DATE_EPOCH \
    -e HOME=/tmp \
    -e PLUGIN_ARTIFACT \
    -e BRIDGE_ARTIFACT \
    -v "${OUT_DIR}:/out" \
    builder bash -c \
    '. /repo/scripts/lib/package.sh \
       && pkg_zip /app/_data/build/stage TakaroTerrariaEvents "/out/${PLUGIN_ARTIFACT}" \
       && pkg_zip /app/_data/build/stage TakaroTerrariaBridge "/out/${BRIDGE_ARTIFACT}"'

# The identity each artifact carries: `takaro-maint artifact validate` reads these files,
# because a zip has no manifest to stamp.
SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"
write_meta() {
    cat > "${OUT_DIR}/${1}.meta.json" <<JSON
{
  "target": "${TERRARIA_TARGET}",
  "fingerprint": "${TERRARIA_FINGERPRINT}",
  "connectorVersion": "${VERSION}",
  "sourceRevision": "${SOURCE_REVISION}",
  "game": "terraria",
  "platform": "tshock",
  "revision": "${TERRARIA_REVISION}",
  "role": "${2}"
}
JSON
}
write_meta "${PLUGIN_ARTIFACT}" plugin
write_meta "${BRIDGE_ARTIFACT}" bridge

echo "  -> ${OUT_DIR}/${PLUGIN_ARTIFACT}"
echo "  -> ${OUT_DIR}/${BRIDGE_ARTIFACT}"
sha256sum "${OUT_DIR}/${PLUGIN_ARTIFACT}" "${OUT_DIR}/${BRIDGE_ARTIFACT}"
