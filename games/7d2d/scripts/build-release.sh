#!/usr/bin/env bash
# Builds the 7D2D mod, embeds <version> into the staged ModInfo.xml, and packages the
# Takaro mod folder as a zip in <out-dir>. Runs locally and in CI.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: build-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir>}"

mkdir -p "$OUT_DIR"
echo "Building 7D2D connector v${VERSION}..."
./scripts/build-mod.sh

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

cp -r _data/build/Mods/Takaro "$STAGE/Takaro"
sed -i "s|<Version value=\"[^\"]*\" />|<Version value=\"${VERSION}\" />|" "$STAGE/Takaro/ModInfo.xml"

( cd "$STAGE" && zip -r takaro-7d2d-mod.zip Takaro >/dev/null )
cp "$STAGE/takaro-7d2d-mod.zip" "$OUT_DIR/"

echo "  -> $OUT_DIR/takaro-7d2d-mod.zip"
