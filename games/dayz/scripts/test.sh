#!/usr/bin/env bash
# Runs the DayZ connector's tests inside the pinned toolchain image of a catalog target:
#   - the sidecar's suite (`npm test`: tsc, then node:test) on a staged copy of the source;
#   - the PBO packer's unit tests (header, SHA-1 trailer, byte-identical output) and a pack of
#     the real mod source.
#
#   games/dayz/scripts/test.sh [--target <id>]
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"
dayz_parse_target_flag "$@"
dayz_resolve_target "${TARGET}"

STAGE="${PROJECT_ROOT}/_data/build/test-${DAYZ_FP16}"
rm -rf "${STAGE}"
mkdir -p "${STAGE}/sidecar"
tar -C "${PROJECT_ROOT}/sidecar" \
    --exclude=./node_modules --exclude=./dist --exclude=./state --exclude='*.log' --exclude=./TakaroConfig.txt \
    -cf - . | tar -C "${STAGE}/sidecar" -xf -
cp -R "${PROJECT_ROOT}/mod" "${STAGE}/mod"

docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp -e npm_config_cache=/tmp/npm-cache -e PYTHONDONTWRITEBYTECODE=1 \
    -v "${STAGE}:${STAGE}" \
    -w "${STAGE}" \
    "${DAYZ_TOOLCHAIN}" \
    bash -c 'set -eu
        echo "== sidecar: npm ci && npm test"
        (cd sidecar && npm ci --no-audit --no-fund && npm test)
        echo "== mod: pack_pbo unit tests"
        python3 -m unittest discover -s mod/tools -p "test_*.py"
        echo "== mod: pack the mod source"
        python3 -I mod/tools/pack_pbo.py mod/takaro_integration pack-check/TakaroIntegration.pbo \
            --prefix TakaroIntegration --timestamp 315532800 >/dev/null
        test -s pack-check/TakaroIntegration.pbo'
rm -rf "${STAGE}"
