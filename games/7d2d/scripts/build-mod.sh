#!/usr/bin/env bash
# Compiles the Takaro mod against one catalog target's reference assemblies.
#
# Usage: build-mod.sh [--target <catalog target id>]
#
# Deploying into the dev rig is dev-servers/scripts/deploy-connector.sh 7d2d, which
# builds and deploys the artifact the ledger records.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

case "${1:-}" in
    build) shift ;;
    deploy)
        echo "build-mod.sh no longer deploys. Use: dev-servers/scripts/deploy-connector.sh 7d2d" >&2
        exit 2
        ;;
esac

sevend2d_parse_target_flag "$@"
sevend2d_resolve_target "${TARGET}"

cd "${PROJECT_ROOT}"
API_DEFINES=$(sevend2d_api_defines)
# The game ships Harmony, Newtonsoft.Json and the rest (Mods/0_TFP_Harmony, the Managed
# folder), and the csproj never copies those. What it does not ship is merged into
# Takaro.dll and internalized, so the mod folder holds one assembly and cannot clash with
# another mod's copy of the same library.
MERGED="_data/7dtd-binaries/LiteDB.dll _data/7dtd-binaries/System.Buffers.dll _data/7dtd-binaries/websocket-sharp.dll"
echo "Building the Takaro mod for ${SEVEND2D_TARGET} (${SEVEND2D_FP16}, API defines: ${API_DEFINES:-none})..."
# As the calling user, so the build output in _data/ stays manageable without sudo.
docker compose run --rm --build \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    builder bash -c \
    "msbuild mod/Takaro.sln /p:Configuration=Release /p:Deterministic=true /p:DebugType=none /p:SevenD2DApiDefines=${API_DEFINES} \
     && mono /opt/ilrepack/ILRepack.exe /internalize /lib:_data/7dtd-binaries /out:bin/Mods/Takaro/Takaro.dll \
          bin/Mods/Takaro/Takaro.dll ${MERGED}"
python3 "${SCRIPT_DIR}/normalize-assembly.py" ./_data/build/Mods/Takaro/Takaro.dll
echo "Build completed successfully."
