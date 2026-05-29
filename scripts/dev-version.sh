#!/usr/bin/env bash
# Prints the dev-build version string for a connector: <last-released>-dev.<shortsha>
# The base version is read from release-please's manifest (the source of truth for
# the last released version per connector). Runs locally and in CI.
set -euo pipefail
cd "$(dirname "$0")/.."

CONNECTOR="${1:?usage: dev-version.sh <rust|minecraft|7d2d>}"
MANIFEST=".release-please-manifest.json"

BASE=$(jq -er --arg c "$CONNECTOR" '.[$c]' "$MANIFEST")
# DEV_SHA lets callers (CI on pull_request) pin the label to the PR head commit instead of
# the ephemeral merge-commit that gets checked out. Local runs fall back to HEAD.
if [ -n "${DEV_SHA:-}" ]; then
  SHA="${DEV_SHA:0:7}"
else
  SHA=$(git rev-parse --short HEAD)
fi

echo "${BASE}-dev.${SHA}"
