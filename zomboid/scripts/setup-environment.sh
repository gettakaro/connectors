#!/usr/bin/env bash
# Stage the Project Zomboid server jar into _deps/ so the agent module can
# compile against the game classes (compileOnly).
#
# Sources, in order of preference:
#   1. the dev-servers bind mount on this host (world-readable, no docker needed)
#   2. `docker exec takaro-dev-zomboid cat …` from the running container
#   3. SteamCMD app 380870 (CI) — not implemented yet, see M3
#
# Idempotent: if _deps/projectzomboid.jar already matches the source sha256 it
# is left alone.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ZOMBOID_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd -- "${ZOMBOID_DIR}/.." && pwd)"

DEPS_DIR="${ZOMBOID_DIR}/_deps"
JAR="${DEPS_DIR}/projectzomboid.jar"
SHA_FILE="${JAR}.sha256"

BIND_MOUNT_JAR="${REPO_ROOT}/dev-servers/_data/zomboid/server/java/projectzomboid.jar"
CONTAINER="${ZOMBOID_CONTAINER:-takaro-dev-zomboid}"
CONTAINER_JAR="/home/steam/ZomboidDedicatedServer/java/projectzomboid.jar"

mkdir -p "${DEPS_DIR}"

stage_from_bind_mount() {
    [ -r "${BIND_MOUNT_JAR}" ] || return 1
    echo "==> staging from bind mount ${BIND_MOUNT_JAR}"
    cp "${BIND_MOUNT_JAR}" "${JAR}.tmp"
}

stage_from_container() {
    command -v docker >/dev/null 2>&1 || return 1
    docker inspect "${CONTAINER}" >/dev/null 2>&1 || return 1
    echo "==> staging from container ${CONTAINER}:${CONTAINER_JAR}"
    docker exec "${CONTAINER}" cat "${CONTAINER_JAR}" > "${JAR}.tmp"
}

if [ -f "${JAR}" ] && [ -f "${SHA_FILE}" ] && [ -r "${BIND_MOUNT_JAR}" ]; then
    have="$(cut -d' ' -f1 < "${SHA_FILE}")"
    want="$(sha256sum "${BIND_MOUNT_JAR}" | cut -d' ' -f1)"
    if [ "${have}" = "${want}" ]; then
        echo "==> _deps/projectzomboid.jar already up to date"
        echo "    sha256 ${have}"
        exit 0
    fi
    echo "==> staged jar is stale (have ${have}, want ${want}) — restaging"
fi

if ! stage_from_bind_mount && ! stage_from_container; then
    echo "ERROR: no source for projectzomboid.jar (no bind mount, no container)" >&2
    exit 1
fi

[ -s "${JAR}.tmp" ] || { echo "ERROR: staged jar is empty" >&2; rm -f "${JAR}.tmp"; exit 1; }
mv "${JAR}.tmp" "${JAR}"

sha="$(sha256sum "${JAR}" | cut -d' ' -f1)"
printf '%s  projectzomboid.jar\n' "${sha}" > "${SHA_FILE}"

echo "==> ${JAR}"
echo "    size   $(stat -c%s "${JAR}") bytes"
echo "    sha256 ${sha}"
