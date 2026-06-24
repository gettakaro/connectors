#!/usr/bin/env bash
# Builds the Minecraft connector at <version> and collects the Paper / NeoForge / Fabric
# JARs into <out-dir>. Runs locally and in CI.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: build-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir>}"

mkdir -p "$OUT_DIR"
echo "Building Minecraft connector v${VERSION}..."
./gradlew build -Pversion="${VERSION}"

collect() {
  local module="$1"
  local jar
  jar=$(find "${module}/build/libs" -name "takaro-${module}-*.jar" \
    -not -name "*-dev-shadow*" -not -name "*-sources*" 2>/dev/null | head -1)
  [ -n "$jar" ] || { echo "Error: no JAR found for ${module}" >&2; exit 1; }
  cp "$jar" "$OUT_DIR/"
  echo "  -> $OUT_DIR/$(basename "$jar")"
}

collect paper
collect neoforge
collect fabric
