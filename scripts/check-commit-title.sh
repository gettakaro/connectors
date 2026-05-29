#!/usr/bin/env bash
# Validates that a string is a Conventional Commit subject. Used to lint PR titles
# (which become the squash-merge commit that release-please reads). Zero dependencies,
# so it runs identically locally and in CI.
#
#   check-commit-title.sh "feat(rust): add reconnect backoff"
set -euo pipefail

TITLE="${1:?usage: check-commit-title.sh \"<title>\"}"

# type(scope)!: description  — scope and ! are optional.
PATTERN='^(feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert)(\([a-zA-Z0-9 ,._/-]+\))?(!)?: .+'

if [[ "$TITLE" =~ $PATTERN ]]; then
  echo "OK: '$TITLE'"
  exit 0
fi

cat >&2 <<EOF
Invalid commit title: '$TITLE'

Titles must follow Conventional Commits, e.g.:
  feat(rust): add reconnect backoff
  fix(7d2d): handle null player on disconnect
  chore: bump dependencies

Allowed types: feat, fix, docs, style, refactor, perf, test, build, ci, chore, revert
(append '!' after the type/scope for a breaking change)
EOF
exit 1
