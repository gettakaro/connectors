#!/usr/bin/env bash
set -e

RUST_DIR="/rust"

# Source Carbon environment (sets LD_PRELOAD, DOORSTOP_ENABLED, etc.)
if [ -f "${RUST_DIR}/carbon/tools/environment.sh" ]; then
    source "${RUST_DIR}/carbon/tools/environment.sh"
    echo "[Carbon] Environment loaded"
fi

# Build startup arguments as an array
ARGS=(
    -batchmode
    -nographics
    +server.port "${RUST_SERVER_PORT:-28015}"
    +rcon.port "${RUST_RCON_PORT:-28016}"
    +rcon.web 1
    +rcon.password "${RCON_PASSWORD:-takaro123}"
    +server.hostname "${RUST_SERVER_NAME:-Takaro Dev}"
    +server.seed "${RUST_SERVER_SEED:-12345}"
    +server.worldsize "${RUST_SERVER_WORLDSIZE:-1000}"
    +server.maxplayers "${RUST_SERVER_MAXPLAYERS:-10}"
    +server.identity "${RUST_SERVER_IDENTITY:-takaro}"
    +server.secure 0
    +server.encryption 0
)

echo "[Takaro] Starting Rust server..."
exec "${RUST_DIR}/RustDedicated" "${ARGS[@]}" 2>&1
