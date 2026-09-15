#!/bin/bash
set -eox pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

cd "${PROJECT_ROOT}"

echo "Setting up 7D2D mod development environment..."

# Default stays the live-proven V3.2.0 (b10) build, Steam buildid 24994542 —
# the build the 2026-09-14 hard test proved the mod on. Override to build against a
# different game build (e.g. testing whether the mod still works on a newer
# release); an override asserts nothing about that build being proven.
EXPECTED_ASSEMBLY_CSHARP_SHA256="${EXPECTED_ASSEMBLY_CSHARP_SHA256:-3737eedc9f143d428a69030317428c45d342be4f11006a46d72bcd7b1d0fc64d}"

verify_managed_assembly() {
  printf '%s  %s\n' \
    "${EXPECTED_ASSEMBLY_CSHARP_SHA256}" \
    "./_data/7dtd-binaries/Assembly-CSharp.dll" \
    | sha256sum --check --status
}

# Create directory structure
mkdir -p ./_data/{7dtd-binaries,build,game-files,lib,ServerFiles}
# Make everything world-writable
# Root-owned files written by containers cannot be chmod-ed by the host user;
# keep the mode fixup best-effort so setup never aborts on them.
chmod -R 777 ./_data 2>/dev/null \
  || echo "warning: some ./_data entries could not be chmod-ed (root-owned); continuing"

# Skip everything if the binaries are already in place (CI cache hit, or a
# repeat local run). The mod build only needs _data/7dtd-binaries/.
if verify_managed_assembly && \
   [ -f "./_data/7dtd-binaries/websocket-sharp.dll" ] && \
   [ -f "./_data/7dtd-binaries/LiteDB.dll" ] && \
   [ -f "./_data/7dtd-binaries/System.Buffers.dll" ]; then
  echo "Binaries already present at ./_data/7dtd-binaries/ — skipping setup."
  exit 0
fi

# Download 7D2D dedicated server files via SteamCMD (with retry).
# "Missing configuration" is a known transient SteamCMD failure where the
# dependency app-manifest fetch races with the self-update; it typically
# clears on a second or third attempt.
echo "Downloading 7D2D server files via SteamCMD..."
attempts=3
for i in $(seq 1 "$attempts"); do
  echo "SteamCMD attempt $i/$attempts"
  if docker compose run --rm steamcmd; then
    break
  fi
  if [ "$i" -lt "$attempts" ]; then
    wait=$((i * 30))
    echo "SteamCMD failed; retrying in ${wait}s..."
    sleep "$wait"
  else
    echo "SteamCMD failed after $attempts attempts" >&2
    exit 1
  fi
done

# Verify that the server files were downloaded
# SteamCMD can sometimes fail silently, this makes the error obvious
if [ ! -d "./_data/game-files/7DaysToDieServer_Data" ]; then
  echo "Error: 7D2D server files not found. Please check the SteamCMD output."
  echo "Contents of _data/"
  ls -l ./_data
  echo "Contents of _data/game-files:"
  ls -l ./_data/game-files
  exit 1
fi

# Verify that the required DLLs are present
if [ ! -d "./_data/game-files/7DaysToDieServer_Data/Managed" ]; then
  echo "Error: Managed DLLs not found. Please check the server files."
  exit 1
fi

# Extract necessary DLLs from game files
echo "Extracting game DLLs..."
docker compose run --rm builder bash -c "cp -f /app/_data/game-files/7DaysToDieServer_Data/Managed/*.dll /app/_data/7dtd-binaries/ && \
                                         cp -f /app/_data/game-files/Mods/0_TFP_Harmony/*.dll /app/_data/7dtd-binaries/"

if ! verify_managed_assembly; then
  echo "Error: Assembly-CSharp.dll does not match live-proven 7D2D V3.2.0 (b10) build 24994542." >&2
  exit 1
fi

# Build dependencies (websocket-sharp)
echo "Preparing dependencies..."
docker compose run --rm deps

echo "Environment setup complete!"
echo "You can now use build-mod.sh to compile the mod."
