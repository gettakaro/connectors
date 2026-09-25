#!/usr/bin/env bash
# Build the single, self-contained Linux VEIN connector release archive.
# Usage: build-release.sh [version] [out-dir]
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
VERSION="${1:-$(tr -d '[:space:]' < "$ROOT/version.txt")}"
OUT_DIR="${2:-$ROOT/dist}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

PLUGIN_DIR="$ROOT/mod"
[ -d "$PLUGIN_DIR" ] || PLUGIN_DIR="$ROOT/plugin"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

"$PLUGIN_DIR/build.sh"
PKG="$STAGE/TakaroVein"
mkdir -p "$PKG/licenses"
mkdir -p "$PKG/scripts"
install -m 0755 "$PLUGIN_DIR/dist/libtakaro-vein.so" "$PKG/libtakaro-vein.so"
install -m 0644 "$ROOT/.env.example" "$PKG/.env.example"
install -m 0644 "$ROOT/docker-compose.example.yml" "$PKG/docker-compose.example.yml"
install -m 0644 "$ROOT/INSTALL.md" "$PKG/INSTALL.md"
install -m 0644 "$PLUGIN_DIR/third_party/README.md" "$PKG/THIRD-PARTY.md"
cp "$PLUGIN_DIR"/third_party/licenses/* "$PKG/licenses/"
install -m 0755 "$ROOT/scripts/smoke-server.py" "$PKG/scripts/smoke-server.py"
install -m 0755 "$ROOT/scripts/drain-legacy.py" "$PKG/scripts/drain-legacy.py"
install -m 0755 "$ROOT/scripts/compare-native-log-grammar.py" "$PKG/scripts/compare-native-log-grammar.py"
install -m 0755 "$PLUGIN_DIR/dist/native-log-probe" "$PKG/scripts/native-log-probe"
install -m 0644 "$PLUGIN_DIR/tests/native_log_legacy_fixtures.json" "$PKG/scripts/native_log_legacy_fixtures.json"
printf '%s\n' "$VERSION" > "$PKG/VERSION"
( cd "$PKG" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS )

# A previous release run may have populated this output directory. Never leave
# a stale sidecar archive beside the native-only release asset.
rm -f "$OUT_DIR/takaro-vein-sidecar.tar.gz"
tar -czf "$OUT_DIR/takaro-vein-plugin.tar.gz" -C "$STAGE" TakaroVein
( cd "$OUT_DIR" && sha256sum takaro-vein-plugin.tar.gz > SHA256SUMS )
cat "$OUT_DIR/SHA256SUMS"
