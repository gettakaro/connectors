#!/usr/bin/env bash
# Stage the Project Zomboid server jar into _deps/ so the agent module can
# compile against the game classes (compileOnly).
#
# Sources, in order of preference:
#   1. the dev-servers bind mount on this host (world-readable, no docker needed)
#   2. `docker exec takaro-dev-zomboid cat …` from the running container
#   3. SteamCMD app 380870 branch `public` (CI; bootstraps steamcmd if missing)
#
# Idempotent: if _deps/projectzomboid.jar already matches the source sha256 it
# is left alone. The expected sha of the live-proven build is pinned below and
# can be overridden with EXPECTED_ZOMBOID_JAR_SHA256; a mismatch WARNS (re-pin
# the hooks with dump-signatures.py after a PZ update) rather than failing.
set -euo pipefail

# Live-proven build 42.20.4 b0bbce05d5. Override to build against another build.
EXPECTED_ZOMBOID_JAR_SHA256="${EXPECTED_ZOMBOID_JAR_SHA256:-80e405a4bfc42f6072e75b3735f458a6514143da011d3226007ded305a442f44}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ZOMBOID_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd -- "${ZOMBOID_DIR}/../.." && pwd)"

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

# CI path: download the PZ dedicated server (branch public) with SteamCMD and
# copy out java/projectzomboid.jar. Bootstraps a local steamcmd if none is on
# PATH. Retries the known transient "manifest" failures.
stage_from_steamcmd() {
    local steam_root="${DEPS_DIR}/steamcmd" steamcmd_bin install_dir="${DEPS_DIR}/pz-server"
    steamcmd_bin="$(command -v steamcmd || true)"
    if [ -z "${steamcmd_bin}" ]; then
        if [ ! -x "${steam_root}/steamcmd.sh" ]; then
            echo "==> bootstrapping SteamCMD into ${steam_root}"
            mkdir -p "${steam_root}"
            curl -sSL https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz \
                | tar -xz -C "${steam_root}" || return 1
        fi
        steamcmd_bin="${steam_root}/steamcmd.sh"
    fi
    command -v curl >/dev/null 2>&1 || { echo "curl needed for SteamCMD bootstrap" >&2; return 1; }
    echo "==> staging via SteamCMD (app 380870, branch public)"
    local i
    for i in 1 2 3; do
        if "${steamcmd_bin}" +force_install_dir "${install_dir}" +login anonymous \
                +app_update 380870 -beta public validate +quit; then
            break
        fi
        echo "   SteamCMD attempt ${i} failed; retrying..." >&2
        sleep $((i * 20))
    done
    [ -r "${install_dir}/java/projectzomboid.jar" ] || return 1
    cp "${install_dir}/java/projectzomboid.jar" "${JAR}.tmp"
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

if ! stage_from_bind_mount && ! stage_from_container && ! stage_from_steamcmd; then
    echo "ERROR: no source for projectzomboid.jar (no bind mount, no container, SteamCMD failed)" >&2
    exit 1
fi

[ -s "${JAR}.tmp" ] || { echo "ERROR: staged jar is empty" >&2; rm -f "${JAR}.tmp"; exit 1; }
mv "${JAR}.tmp" "${JAR}"

sha="$(sha256sum "${JAR}" | cut -d' ' -f1)"
printf '%s  projectzomboid.jar\n' "${sha}" > "${SHA_FILE}"

if [ "${sha}" != "${EXPECTED_ZOMBOID_JAR_SHA256}" ]; then
    echo "WARNING: staged projectzomboid.jar sha256 ${sha} != pinned ${EXPECTED_ZOMBOID_JAR_SHA256}" >&2
    echo "         PZ may have updated — re-pin the hooks with scripts/dump-signatures.py." >&2
fi

echo "==> ${JAR}"
echo "    size   $(stat -c%s "${JAR}") bytes"
echo "    sha256 ${sha}"
