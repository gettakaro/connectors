#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v node &> /dev/null; then
    echo "Error: Node.js is required but not found." >&2
    echo "Install it from https://nodejs.org/ (v22+ required for built-in WebSocket)" >&2
    exit 1
fi

RCON_PASS="${RCON_PASSWORD:-takaro123}"
RCON_PORT="${RCON_PORT:-28016}"
RCON_HOST="${RCON_HOST:-localhost}"

COMMAND="${1:-c.reload TakaroConnector}"

echo "Sending RCON command: $COMMAND"
node -e "
const command = process.argv[1];
const ws = new WebSocket('ws://${RCON_HOST}:${RCON_PORT}/${RCON_PASS}');
ws.onopen = () => {
    ws.send(JSON.stringify({Identifier: 1, Message: command}));
};
ws.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    if (msg.Identifier === 1) {
        console.log(msg.Message || '(empty response)');
        ws.close();
        process.exit(0);
    }
};
ws.onerror = (err) => {
    console.error('RCON error:', err.message || 'connection failed');
    process.exit(1);
};
setTimeout(() => { console.error('RCON connection timed out after 5s'); process.exit(1); }, 5000);
" -- "$COMMAND" 2>&1 || echo "Warning: Could not connect to RCON. Is the server running?"
