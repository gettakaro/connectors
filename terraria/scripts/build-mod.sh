#!/usr/bin/env bash
# Builds the server-side TShock plugin into terraria/_data/build.
set -euo pipefail
cd "$(dirname "$0")/.."

REFS_DIR="${TERRARIA_REFS_DIR:-_data/refs}"
OUT_DIR="${TERRARIA_BUILD_DIR:-_data/build/TakaroTerrariaEvents}"
PROJECT="src/TakaroTerrariaEvents/TakaroTerrariaEvents.csproj"
DOTNET_IMAGE="${TERRARIA_DOTNET_IMAGE:-mcr.microsoft.com/dotnet/sdk:9.0}"

if [ ! -f "$REFS_DIR/TShockAPI.dll" ]; then
  echo "Missing TShock reference DLLs in terraria/$REFS_DIR" >&2
  echo "Run terraria/scripts/setup-environment.sh first." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

if command -v dotnet >/dev/null 2>&1 && dotnet --list-sdks | awk '{print $1}' | grep -q '^9\.'; then
  dotnet publish "$PROJECT" \
    -c Release \
    -o "$OUT_DIR" \
    -p:TShockReferencePath="$(realpath "$REFS_DIR")"
else
  REPO_ROOT="$(cd .. && pwd)"

  docker run --rm \
    -v "$REPO_ROOT:$REPO_ROOT" \
    -w "$REPO_ROOT/terraria" \
    "$DOTNET_IMAGE" \
    dotnet publish "$PROJECT" \
      -c Release \
      -o "$OUT_DIR" \
      -p:TShockReferencePath="$(realpath "$REFS_DIR")"
fi

echo "Built terraria/$OUT_DIR/TakaroTerrariaEvents.dll"
