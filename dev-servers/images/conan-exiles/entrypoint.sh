#!/usr/bin/env bash
# Starts the pinned Conan Exiles dedicated server inside the catalog's image.
#
# Bind-mounted into the container by dev-servers/compose/conan-exiles.yml rather than
# baked into an image: the image is the one the catalog pins by digest, and nothing here
# may change the bytes it runs. There is no Steam client and no updater in this file --
# the game files are installed once by `takaro-maint install` from the pinned depot
# manifests, and this script refuses to start if they are not there.
set -euo pipefail

INSTALL_DIR="${CONAN_INSTALL_DIR:-/conan}"
CONFIG_DIR="${INSTALL_DIR}/ConanSandbox/Saved/Config/LinuxServer"

if [ ! -f "${INSTALL_DIR}/ConanSandboxServer.sh" ]; then
    echo "[conan] ERROR: ${INSTALL_DIR}/ConanSandboxServer.sh is missing." >&2
    echo "[conan] The pinned build has not been installed here yet:" >&2
    echo "[conan]   dev-servers/scripts/install.sh conan-exiles" >&2
    exit 1
fi

mkdir -p "${CONFIG_DIR}" "${INSTALL_DIR}/ConanSandbox/Saved/Logs"

# RCON is for manual admin access to the dev server (the native connector does not use
# it), published on 127.0.0.1:25580. RconMaxKarma is raised because Conan throttles
# repeated RCON. The password lives here and never on the command line, which is logged
# and inspectable.
#
# The section is rewritten on every start rather than written once: RCON_PASSWORD comes
# from dev-servers/.env, and a Game.ini left alone would keep the server on the old secret
# after the .env changed, failing RCON auth with nothing saying why.
GAME_INI="${CONFIG_DIR}/Game.ini"
echo "[conan] Writing [RconPlugin] settings to ${GAME_INI}"
GAME_INI_NEW="${GAME_INI}.takaro-new"
REST=""
if [ -f "${GAME_INI}" ]; then
    # Everything except the [RconPlugin] section; the substitution drops trailing blank
    # lines, so restarting does not grow the file.
    REST="$(awk '/^\[RconPlugin\]/ { skip = 1; next } /^\[/ { skip = 0 } !skip' "${GAME_INI}")"
fi
: > "${GAME_INI_NEW}"
if [ -n "${REST}" ]; then
    printf '%s\n\n' "${REST}" > "${GAME_INI_NEW}"
fi
cat >> "${GAME_INI_NEW}" <<INI
[RconPlugin]
RconEnabled=1
RconPassword=${RCON_PASSWORD}
RconPort=${CONAN_RCON_PORT:-25575}
RconMaxKarma=1000
INI
chmod 600 "${GAME_INI_NEW}"
mv "${GAME_INI_NEW}" "${GAME_INI}"

echo "[conan] Starting the pinned Conan Exiles dedicated server..."
cd "${INSTALL_DIR}"
exec ./ConanSandboxServer.sh \
    -log \
    -server \
    -nosteamclient \
    -Port="${CONAN_GAME_PORT:-7777}" \
    -QueryPort="${CONAN_QUERY_PORT:-27015}" \
    -RconEnabled=1 \
    -RconPort="${CONAN_RCON_PORT:-25575}"
