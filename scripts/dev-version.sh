#!/usr/bin/env bash
# Prints the dev-build version string for a connector: <last-released>-dev.<shortsha>
# The base version is read from release-please's manifest (the source of truth for
# the last released version per connector). Runs locally and in CI.
set -euo pipefail
cd "$(dirname "$0")/.."

CONNECTOR="${1:?usage: dev-version.sh <rust|minecraft|7d2d>}"
MANIFEST=".release-please-manifest.json"

BASE=$(jq -er --arg c "$CONNECTOR" '.[$c]' "$MANIFEST")
SHA=$(git rev-parse --short HEAD)

echo "${BASE}-dev.${SHA}"
