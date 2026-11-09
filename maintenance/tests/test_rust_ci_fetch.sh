#!/usr/bin/env bash
# ci-fetch-references.sh with a stand-in setup-environment.sh: which Steam failures a pull
# request may skip past, and that the skip applies only to pull requests changing no Rust file.
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
GUARD="$REPO_ROOT/games/rust/scripts/ci-fetch-references.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
FAILED=0

# Already-resolved values: lib-target.sh takes them from the environment and never needs uv.
export RUST_TARGET=carbon-100 RUST_FINGERPRINT=abc RUST_STEAM_APP=258550 RUST_STEAM_BRANCH=public RUST_STEAM_BUILDID=100

mkdir -p "$WORK/steam"
cat >"$WORK/steam/258550" <<'JSON'
{"data": {"258550": {"depots": {"branches": {"public": {"buildid": "200"}}}}}}
JSON

cat >"$WORK/setup" <<'STUB'
#!/usr/bin/env bash
case "$FAKE_SETUP" in
  ok) exit 0 ;;
  not-served) printf '{\n  "ok": false,\n  "reason": "manifest-not-served"\n}\n'; exit 4 ;;
  other-upstream) printf '{\n  "ok": false\n}\n'; exit 4 ;;
  compile) exit 6 ;;
esac
STUB
chmod +x "$WORK/setup"

# expect <what> <setup mode> <exit> <skip> <pin_unavailable> <grep for the log, or -> <changed path>...
expect() {
  local what="$1" mode="$2" want_exit="$3" want_skip="$4" want_pin="$5" want_log="$6"
  shift 6
  printf '%s\n' "$@" >"$WORK/changed"
  : >"$WORK/output"
  local got=0 log
  log="$(FAKE_SETUP="$mode" RUST_SETUP_ENVIRONMENT="$WORK/setup" RUST_CI_CHANGED_PATHS="$WORK/changed" \
    STEAM_INFO_URL="${INFO_URL:-file://$WORK/steam}" GITHUB_OUTPUT="$WORK/output" bash "$GUARD" 2>&1)" || got=$?
  local skip pin
  skip="$(sed -n 's/^skip=//p' "$WORK/output")"
  pin="$(sed -n 's/^pin_unavailable=//p' "$WORK/output")"
  if [ "$got" != "$want_exit" ] || [ "$skip" != "$want_skip" ] || [ "$pin" != "$want_pin" ]; then
    echo "FAIL $what: exit=$got skip=[$skip] pin_unavailable=[$pin]; wanted $want_exit [$want_skip] [$want_pin]" >&2
    echo "$log" >&2
    FAILED=1
    return 0
  fi
  if [ "$want_log" != "-" ] && ! grep -qF "$want_log" <<<"$log"; then
    echo "FAIL $what: the log does not say '$want_log'" >&2
    echo "$log" >&2
    FAILED=1
    return 0
  fi
  echo "PASS $what"
}

expect "not served, maintenance-only pull request skips with a warning naming the target and build" \
  not-served 0 true true "carbon-100 (Steam build 100)" maintenance/src/takaro_maint/cli.py
expect "the warning names the newer public build" \
  not-served 0 true true "newer public build is 200" scripts/lib/target.sh
expect "not served, a games/rust change fails" not-served 4 "" "" - games/rust/README.md
expect "not served, a catalog/rust change fails" not-served 4 "" "" - catalog/rust/targets/carbon-100.json
expect "not served, one Rust path among many still fails" \
  not-served 4 "" "" - maintenance/README.md games/rust/README.md
expect "another upstream failure is not tolerated" other-upstream 4 "" "" - maintenance/README.md
expect "a compile failure is not tolerated" compile 6 "" "" - maintenance/README.md
expect "a warm cache runs the checks; the release legs are skipped while the pin is stale" \
  ok 0 false true "behind the public build 200" maintenance/README.md
expect "a warm cache on a Rust pull request leaves the release legs on" ok 0 false false - games/rust/README.md

export RUST_STEAM_BUILDID=300
expect "a lookup behind the pin does not skip the release legs" ok 0 false false - maintenance/README.md

export RUST_STEAM_BUILDID=200
expect "Steam refusing its own public head is not a moved head" \
  not-served 4 "" "" "still the public head" maintenance/README.md
expect "a current pin leaves the release legs on" ok 0 false false - maintenance/README.md
export RUST_STEAM_BUILDID=100

INFO_URL="file://$WORK/no-such-dir" expect "the skip survives a failed head lookup, naming it" \
  not-served 0 true true "unknown (the Steam app-info lookup failed)" maintenance/README.md

# Changes that cannot be read are treated as Rust changes.
if ( cd "$WORK"
  FAKE_SETUP=not-served RUST_SETUP_ENVIRONMENT="$WORK/setup" RUST_CI_CHANGED_PATHS="$WORK/no-such-file" \
    STEAM_INFO_URL="file://$WORK/steam" GITHUB_OUTPUT="$WORK/output" bash "$GUARD" >/dev/null 2>&1 ); then
  echo "FAIL unreadable changes must fail" >&2
  FAILED=1
else
  echo "PASS unreadable changes fail"
fi

[ "$FAILED" -eq 0 ] || exit 1
echo "ALL PASS"
