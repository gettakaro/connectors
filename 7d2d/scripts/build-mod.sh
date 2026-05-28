#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

cd "${PROJECT_ROOT}"

MODE=${1:-build} # Can be 'build' or 'deploy'

echo "Building Takaro mod..."

# Compile the mod
docker compose run --rm builder bash -c "msbuild Takaro.sln /p:Configuration=Release"

echo "Build completed successfully!"

# Deploy the mod if requested
if [ "$MODE" == "deploy" ]; then
  echo "Deploying mod to game server..."

  # Deploy to server
  mkdir -p ./_data/ServerFiles/Mods/Takaro
  cp -r ./_data/build/Mods/Takaro/* ./_data/ServerFiles/Mods/Takaro/
  echo "Mod deployed to game server."

  # Optional: restart server if running
  if docker compose ps -q 7dtdserver &>/dev/null; then
    echo "Restarting game server to apply changes..."
    docker compose restart 7dtdserver
  fi
fi
