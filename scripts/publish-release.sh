#!/usr/bin/env bash
# Thin wrapper around `gh` for publishing connector releases. Usable locally (with an
# authenticated gh) or in CI (with GH_TOKEN set).
#
#   publish-release.sh upload  <tag> <file...>
#       Attach artifacts to an existing release (used for stable releases that
#       release-please already created).
#
#   publish-release.sh rolling <tag> <title> <notes> <file...>
#       Recreate a rolling pre-release at the current commit (used for dev builds).
#       Deletes the old tag/release so the tag always points at the latest build.
set -euo pipefail

MODE="${1:?usage: publish-release.sh <upload|rolling> ...}"
shift

case "$MODE" in
  upload)
    TAG="${1:?tag required}"; shift
    gh release upload "$TAG" "$@" --clobber
    ;;
  rolling)
    TAG="${1:?tag required}"; TITLE="${2:?title required}"; NOTES="${3:?notes required}"; shift 3
    gh release delete "$TAG" --cleanup-tag --yes 2>/dev/null || true
    gh release create "$TAG" "$@" \
      --prerelease \
      --title "$TITLE" \
      --notes "$NOTES" \
      --target "${GITHUB_SHA:-$(git rev-parse HEAD)}"
    ;;
  *)
    echo "Error: unknown mode '$MODE' (expected 'upload' or 'rolling')" >&2
    exit 1
    ;;
esac
