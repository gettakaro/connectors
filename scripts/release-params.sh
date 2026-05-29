#!/usr/bin/env bash
# Resolves build/publish parameters for a connector based on the CI trigger.
# Reads context from env: EVENT (github.event_name), IN_VERSION/IN_TAG (workflow_call inputs).
# Prints GITHUB_OUTPUT-style key=value lines:
#   version  - version string to embed in the artifact
#   tag      - release tag (empty when nothing is published)
#   publish  - stable | rolling | none
#
# Modes:
#   stable  - called by release-please for a real release (IN_TAG provided)
#   rolling - a normal push to main -> update the rolling <connector>-dev pre-release
#   none    - pull_request, or a release-please release commit (build only, no publish)
set -euo pipefail
cd "$(dirname "$0")/.."

CONNECTOR="${1:?usage: release-params.sh <connector>}"

if [ -n "${IN_TAG:-}" ]; then
  VERSION="${IN_VERSION:?IN_VERSION must be set when IN_TAG is set}"
  TAG="${IN_TAG}"
  PUBLISH="stable"
elif [ "${EVENT:-}" = "push" ] && ! (git log -1 --pretty=%s | grep -qiE '^chore.*release'); then
  VERSION="$(scripts/dev-version.sh "$CONNECTOR")"
  TAG="${CONNECTOR}-dev"
  PUBLISH="rolling"
else
  VERSION="$(scripts/dev-version.sh "$CONNECTOR")"
  TAG=""
  PUBLISH="none"
fi

echo "version=${VERSION}"
echo "tag=${TAG}"
echo "publish=${PUBLISH}"
