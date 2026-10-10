#!/usr/bin/env bash
# Runs the Generic Connector contract harness against one catalog target's assemblies.
#
# Usage: test-contract.sh [--target <catalog target id>]
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
FIXTURE="${PROJECT_ROOT}/tests/fixtures/generic-protocol.json"
LEGACY_FIXTURE="${PROJECT_ROOT}/tests/fixtures/legacy-parity.json"
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

# The builder mounts the target's reference directory, so the target is resolved first.
sevend2d_parse_target_flag "$@"
sevend2d_resolve_target "${TARGET}"
API_DEFINES=$(sevend2d_api_defines)

docker compose --project-directory "${PROJECT_ROOT}" run --rm --no-deps \
  --volume "${FIXTURE}:/tmp/generic-protocol.json:ro" \
  --volume "${LEGACY_FIXTURE}:/tmp/legacy-parity.json:ro" \
  -e API_DEFINES="${API_DEFINES}" \
  builder bash -lc '
    set -euo pipefail
    test_dir=$(mktemp -d)
    trap '\''rm -rf -- "$test_dir"'\'' EXIT
    mcs -langversion:latest ${API_DEFINES:+-define:${API_DEFINES}} \
      -out:"$test_dir/contract-harness.exe" \
      -r:/usr/lib/mono/msbuild/Current/bin/Newtonsoft.Json.dll \
      /app/mod/src/WebSocket/WebSocketMessage.cs \
      /app/mod/src/WebSocket/ProtocolHandshake.cs \
      /app/mod/src/WebSocket/GameEventPublisher.cs \
      /app/mod/src/WebSocket/RequestRouter.cs \
      /app/mod/src/WebSocket/ReadHandlers.cs \
      /app/mod/src/WebSocket/GiveItemHandler.cs \
      /app/mod/src/Services/BanExpiry.cs \
      /app/mod/src/Services/BanIdentity.cs \
      /app/mod/src/Services/ConsoleCommandOutcome.cs \
      /app/mod/src/Services/ProtocolDiagnostics.cs \
      /app/mod/src/Services/OutboundLedger.cs \
      /app/mod/src/Services/PlayerLocationReadWindow.cs \
      /app/mod/src/Services/ServerMessageEchoGuard.cs \
      /app/mod/src/Services/MapCatalog.cs \
      /app/mod/src/Services/PlayerProximateItemDelivery.cs \
      /app/mod/src/Shared.cs \
      /app/tests/ContractHarness.cs
    MONO_PATH=/usr/lib/mono/msbuild/Current/bin \
      mono "$test_dir/contract-harness.exe" /tmp/generic-protocol.json /tmp/legacy-parity.json
  '
