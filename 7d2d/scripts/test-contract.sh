#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
FIXTURE="${PROJECT_ROOT}/tests/fixtures/generic-protocol.json"

docker compose --project-directory "${PROJECT_ROOT}" run --rm --no-deps \
  --volume "${FIXTURE}:/tmp/generic-protocol.json:ro" \
  builder bash -lc '
    set -euo pipefail
    test_dir=$(mktemp -d)
    trap '\''rm -rf -- "$test_dir"'\'' EXIT
    mcs -langversion:latest \
      -out:"$test_dir/contract-harness.exe" \
      -r:/usr/lib/mono/msbuild/Current/bin/Newtonsoft.Json.dll \
      /app/src/WebSocket/WebSocketMessage.cs \
      /app/src/WebSocket/RequestRouter.cs \
      /app/src/WebSocket/ReadHandlers.cs \
      /app/src/WebSocket/GiveItemHandler.cs \
      /app/src/Services/PlayerProximateItemDelivery.cs \
      /app/src/Shared.cs \
      /app/tests/ContractHarness.cs
    MONO_PATH=/usr/lib/mono/msbuild/Current/bin \
      mono "$test_dir/contract-harness.exe" /tmp/generic-protocol.json
  '
