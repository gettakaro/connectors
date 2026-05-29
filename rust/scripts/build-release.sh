#!/usr/bin/env bash
# Stages the Rust connector for release: embeds <version> into the [Info(...)] attribute
# and copies the plugin source into <out-dir>. Runs locally and in CI.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: build-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir>}"

mkdir -p "$OUT_DIR"
echo "Staging Rust connector v${VERSION}..."

sed "s/\[Info(\"TakaroConnector\", \"Takaro\", \"[^\"]*\")\]/[Info(\"TakaroConnector\", \"Takaro\", \"${VERSION}\")]/" \
  plugin/TakaroConnector.cs > "$OUT_DIR/TakaroConnector.cs"

echo "  -> $OUT_DIR/TakaroConnector.cs"
