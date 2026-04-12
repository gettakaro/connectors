#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PLUGIN_DIR="_data/plugins"
mkdir -p "$PLUGIN_DIR"

echo "Deploying TakaroConnector plugin..."
cp plugin/TakaroConnector.cs "$PLUGIN_DIR/TakaroConnector.cs"
echo "  -> $PLUGIN_DIR/TakaroConnector.cs"
echo "Done. Run ./scripts/reload.sh to hot-reload the plugin."
