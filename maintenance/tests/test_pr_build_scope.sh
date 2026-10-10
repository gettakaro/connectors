#!/usr/bin/env bash
# pr-build-scope.sh against the real repository layout: which changed paths make a pull
# request run a connector's release build. Prints PASS per case and ALL PASS at the end, and
# exits non-zero on the first case that does not behave as stated.
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
SCOPE="$REPO_ROOT/scripts/pr-build-scope.sh"

# expect <true|false> <connector> <path>...
expect() {
  local want="$1" connector="$2"
  shift 2
  local got
  got="$(printf '%s\n' "$@" | "$SCOPE" "$connector" 2>/dev/null)"
  if [ "$got" != "build=$want" ]; then
    echo "FAIL $connector [$*]: expected build=$want, got '$got'" >&2
    exit 1
  fi
  echo "PASS $connector build=$want [$*]"
}

# The connector's own sources, catalog and workflows.
expect true rust games/rust/plugin/Takaro.cs
expect true rust catalog/rust/targets/carbon-25773132.json
expect true rust .github/workflows/rust.yml
expect true conan-exiles .github/workflows/conan-exiles-repin-watch.yml
expect true valheim .github/workflows/connector-release.yml
expect true valheim catalog/schema/v1/target.schema.json

# Shared build tooling.
expect true rust maintenance/src/takaro_maint/steam/depot.py
expect true rust maintenance/src/takaro_maint/games/base.py
expect true rust maintenance/tools.lock.json
expect true rust maintenance/uv.lock
expect true rust scripts/lib/package.sh
expect true rust scripts/publish-release.sh

# The connector's own adapter builds it; another connector's adapter does not.
expect true rust maintenance/src/takaro_maint/games/rust/__init__.py
expect true 7d2d maintenance/src/takaro_maint/games/seven_days/__init__.py
expect false rust maintenance/src/takaro_maint/games/seven_days/__init__.py
expect false valheim maintenance/src/takaro_maint/games/conan_exiles/__init__.py

# Another connector, docs, tests and lint-only paths never do (the 2026-10-07 case: a Conan
# Exiles re-pin that touched maintenance/docs and maintenance/tests built Rust and Valheim).
expect false rust \
  catalog/conan-exiles/targets/linux-25738716.json \
  games/conan-exiles/README.md \
  maintenance/docs/support-policy.md \
  maintenance/tests/test_cli_catalog.py \
  maintenance/tests/test_game_conan_exiles.py
expect false valheim .github/workflows/rust.yml
expect false valheim maintenance/README.md maintenance/config/schedule.yaml
expect false valheim scripts/check-connector-metadata.sh scripts/pr-build-scope.sh
expect false valheim README.md .github/workflows/lint.yml games/connector.schema.json
expect false valheim

# One build input among unrelated paths is enough.
expect true valheim maintenance/docs/release.md games/valheim/README.md

echo "ALL PASS"
