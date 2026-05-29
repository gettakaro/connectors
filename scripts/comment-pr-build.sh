#!/usr/bin/env bash
# Upserts a sticky PR comment linking the disposable per-PR pre-release build for a connector.
# One comment per connector, keyed by a hidden marker so repeated pushes edit it in place
# instead of spamming new comments. Usable locally (authenticated gh) or in CI (GH_TOKEN set).
#
#   comment-pr-build.sh <connector> <pr-number> <tag> <version> <asset-file...>
set -euo pipefail

CONNECTOR="${1:?usage: comment-pr-build.sh <connector> <pr-number> <tag> <version> <asset-file...>}"
PR="${2:?pr-number required}"
TAG="${3:?tag required}"
VERSION="${4:?version required}"
shift 4

REPO="${GH_REPO:-${GITHUB_REPOSITORY:?GITHUB_REPOSITORY or GH_REPO must be set}}"
MARKER="<!-- takaro-pr-build:${CONNECTOR} -->"

LINKS=""
for f in "$@"; do
  base=$(basename "$f")
  LINKS="${LINKS}- [\`${base}\`](https://github.com/${REPO}/releases/download/${TAG}/${base})"$'\n'
done

BODY="${MARKER}
### 🟢 ${CONNECTOR} build for this PR

**Version:** \`${VERSION}\`

${LINKS}
Direct download — no login required. This build is replaced on every push and deleted when the PR closes. _Not for production._"

# Find an existing sticky comment for this connector (first match wins).
EXISTING=$(gh api "repos/${REPO}/issues/${PR}/comments" --paginate \
  | jq -r --arg m "$MARKER" '[.[] | select(.body | startswith($m)) | .id][0] // empty')

if [ -n "$EXISTING" ]; then
  jq -n --arg body "$BODY" '{body: $body}' \
    | gh api "repos/${REPO}/issues/comments/${EXISTING}" -X PATCH --input -
else
  jq -n --arg body "$BODY" '{body: $body}' \
    | gh api "repos/${REPO}/issues/${PR}/comments" -X POST --input -
fi
