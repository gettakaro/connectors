#!/usr/bin/env bash
# Decides whether a pull request has to run one connector's release build.
#
#   git diff --name-only <base> <head> | pr-build-scope.sh <connector>
#
# Prints `build=true` or `build=false` (GITHUB_OUTPUT style) and, on stderr, the path that
# decided it. Only pull_request runs ask: a push to main, a release and a manual run always
# build, so main keeps catching a pin Steam has stopped serving.
#
# A release build is a function of the connector's own sources and catalog, the shared build
# tooling, and the workflows that run it. A pull request that changes none of those would build
# exactly what main last built, so the only thing it could add is an unrelated failure: a pinned
# Steam depot manifest that Steam no longer serves anonymously after the game updated.
#
# Build inputs (any one of these changed means build):
#   games/<connector>/**, catalog/<connector>/**, catalog/schema/**
#   .github/workflows/<connector>.yml, .github/workflows/<connector>-*.yml,
#   .github/workflows/connector-release.yml
#   maintenance/** and scripts/**, except the paths below
# Not build inputs:
#   maintenance/docs/**, maintenance/tests/**, maintenance/README.md, maintenance/config/**
#     (documentation, tests, and the discovery schedule; the tests run in their own jobs)
#   maintenance/src/takaro_maint/games/<another connector's adapter>/**
#   scripts/check-*.sh, scripts/cleanup-pr-builds.sh, scripts/pr-build-scope.sh
#     (lint checks, closed-PR cleanup, and this decision itself)
set -euo pipefail
cd "$(dirname "$0")/.."

CONNECTOR="${1:?usage: pr-build-scope.sh <connector> < changed-paths}"
[ -d "games/$CONNECTOR" ] || { echo "error: no games/$CONNECTOR directory" >&2; exit 2; }

# The adapter package is named after the module, not the catalog id (7d2d -> seven_days).
ADAPTERS="maintenance/src/takaro_maint/games"
OWN_ADAPTER=""
for init in "$ADAPTERS"/*/__init__.py; do
  if grep -qE "^GAME_ID = \"${CONNECTOR}\"$" "$init"; then
    OWN_ADAPTER="$(basename "$(dirname "$init")")"
  fi
done

decide() {
  local path="$1"
  case "$path" in
    "games/$CONNECTOR/"* | "catalog/$CONNECTOR/"* | catalog/schema/*) return 0 ;;
    ".github/workflows/$CONNECTOR.yml" | ".github/workflows/$CONNECTOR-"*.yml) return 0 ;;
    .github/workflows/connector-release.yml) return 0 ;;
    maintenance/docs/* | maintenance/tests/* | maintenance/README.md | maintenance/config/*) return 1 ;;
    "$ADAPTERS/"*/*)
      local adapter="${path#"$ADAPTERS/"}"
      adapter="${adapter%%/*}"
      [ -n "$OWN_ADAPTER" ] && [ "$adapter" = "$OWN_ADAPTER" ] && return 0
      [ -d "$ADAPTERS/$adapter" ] && return 1
      return 0 ;;
    maintenance/*) return 0 ;;
    scripts/check-*.sh | scripts/cleanup-pr-builds.sh | scripts/pr-build-scope.sh) return 1 ;;
    scripts/*) return 0 ;;
    *) return 1 ;;
  esac
}

while IFS= read -r path; do
  [ -n "$path" ] || continue
  if decide "$path"; then
    echo "$CONNECTOR: build input changed: $path" >&2
    echo "build=true"
    exit 0
  fi
done

echo "$CONNECTOR: this pull request changes none of its build inputs" >&2
echo "build=false"
