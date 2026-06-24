#!/usr/bin/env bash
# Deletes the disposable per-PR pre-releases (and their tags) for a closed PR. Idempotent:
# connectors that never produced a build for this PR are skipped silently. Usable locally
# (authenticated gh) or in CI (GH_TOKEN set).
#
#   cleanup-pr-builds.sh <pr-number>
set -euo pipefail

PR="${1:?usage: cleanup-pr-builds.sh <pr-number>}"
REPO="${GH_REPO:-${GITHUB_REPOSITORY:?GITHUB_REPOSITORY or GH_REPO must be set}}"

for connector in rust minecraft 7d2d; do
  TAG="pr-${PR}-${connector}"
  if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    echo "Removing ${TAG}..."
    # Let a real delete failure (auth, rate limit, partial tag removal) fail the job
    # instead of silently orphaning the release.
    gh release delete "$TAG" --repo "$REPO" --cleanup-tag --yes
  else
    echo "No ${TAG} release; skipping."
  fi
done
