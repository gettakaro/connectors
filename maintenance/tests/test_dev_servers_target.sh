#!/usr/bin/env bash
# dev-servers/lib/targets.sh ds_target: a failing tool and a game with no catalog target are
# different answers. Fail-open on the first one would skip every catalog check below and boot
# the rig on whatever happens to be in the data directory.
#
# Each case builds its own fixture holding a copy of the dev-servers library (common.sh sources
# lib/games/*.sh and lib/targets.sh, so the whole directory is copied) and a stub takaro-maint on the
# path the library hard-codes. Run it from anywhere; it prints PASS per case and ALL PASS.
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CASE=0

# A tree with the library and a stub takaro-maint whose behaviour the case chooses.
fixture() {
  local name="$1" mode="$2" root
  root="$WORK/$name"
  mkdir -p "$root/dev-servers" "$root/maintenance/bin"
  cp -r "$REPO_ROOT/dev-servers/lib" "$root/dev-servers/lib"
  case "$mode" in
    target)
      cat >"$root/maintenance/bin/takaro-maint" <<'STUB'
#!/usr/bin/env bash
printf '{"targets": [{"id": "fabric-26.2"}]}\n'
STUB
      ;;
    retired-first)
      cat >"$root/maintenance/bin/takaro-maint" <<'STUB'
#!/usr/bin/env bash
printf '{"targets": [{"id": "fabric-26.1", "status": "retired"}, {"id": "fabric-26.2", "status": "candidate"}]}\n'
STUB
      ;;
    none)
      cat >"$root/maintenance/bin/takaro-maint" <<'STUB'
#!/usr/bin/env bash
printf '{"targets": []}\n'
STUB
      ;;
    fail)
      cat >"$root/maintenance/bin/takaro-maint" <<'STUB'
#!/usr/bin/env bash
echo "catalog is unreadable" >&2
exit 2
STUB
      ;;
  esac
  chmod +x "$root/maintenance/bin/takaro-maint"
  printf '%s\n' "$root"
}

# Source the library with REPO_ROOT pointed at the fixture and print ds_target's answer.
probe() {
  local root="$1"
  REPO_ROOT="$root" bash -c '
    REPO_ROOT="'"$root"'"
    # shellcheck disable=SC1090
    . "$REPO_ROOT/dev-servers/lib/common.sh"
    if value="$(ds_target minecraft-fabric)"; then
      printf "ok:%s\n" "$value"
    else
      printf "failed:%s\n" "$?"
    fi
  '
}

check() {
  local name="$1" want="$2" got="$3"
  CASE=$((CASE + 1))
  if [ "$got" != "$want" ]; then
    echo "FAIL $name: expected '$want', got '$got'" >&2
    exit 1
  fi
  echo "PASS $name"
}

# 1. A game the catalog drives resolves to its target id.
check "a catalog-driven game resolves its target" "ok:fabric-26.2" \
  "$(probe "$(fixture catalog target)" 2>/dev/null)"

# 1b. A retired target listed first is skipped; its depots are usually gone from Steam.
check "a retired target is not picked over a live one" "ok:fabric-26.2" \
  "$(probe "$(fixture retired retired-first)" 2>/dev/null)"

# 2. A successful call with an empty list is the real "this game has no catalog target".
check "no target is an empty answer, not a failure" "ok:" \
  "$(probe "$(fixture empty none)" 2>/dev/null)"

# 3. A tool that fails is not "no target": ds_target reports failure so the caller can stop.
root_fail="$(fixture broken fail)"
check "a failing takaro-maint is a failure, not an empty target" "failed:1" \
  "$(probe "$root_fail" 2>/dev/null)"

# 4. ...and the tool's own stderr reaches the operator rather than /dev/null.
stderr="$(probe "$root_fail" 2>&1 >/dev/null)"
case "$stderr" in
  *"catalog is unreadable"*) echo "PASS the tool's stderr is shown" ;;
  *) echo "FAIL the tool's stderr was swallowed: $stderr" >&2; exit 1 ;;
esac


# ── ds_write_target_env / ds_preflight_target ────────────────────────────────
# The gate decides whether the rig may boot a target-driven game, and the resolve writes
# the environment compose reads. Both call `ds_target` and stop on an unusable answer.

# A tree whose stub takaro-maint records its argv and answers per subcommand.
gate_fixture() {
  local name="$1" listing="$2" ledger_status="$3" root
  root="$WORK/$name"
  mkdir -p "$root/dev-servers" "$root/maintenance/bin"
  cp -r "$REPO_ROOT/dev-servers/lib" "$root/dev-servers/lib"
  cat >"$root/maintenance/bin/takaro-maint" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"$root/argv.log"
case "\$1 \$2" in
  "targets list") printf '%s\n' '$listing' ;;
  "targets resolve") exit 0 ;;
  "ledger check") exit $ledger_status ;;
esac
STUB
  chmod +x "$root/maintenance/bin/takaro-maint"
  printf '%s\n' "$root"
}

# Run one library call against a fixture and report its exit status.
gate() {
  local root="$1" call="$2"
  REPO_ROOT="$root" bash -c '
    REPO_ROOT="'"$root"'"
    # shellcheck disable=SC1090
    . "$REPO_ROOT/dev-servers/lib/common.sh"
    DS_DATA="$REPO_ROOT/_data"
    # ds_die exits the shell, so the call runs in a subshell whose status is readable.
    if ( '"$call"' ); then printf "ok\n"; else printf "died:%s\n" "$?"; fi
  '
}

TARGET_LISTING='{"targets": [{"id": "fabric-26.2"}]}'
NO_TARGET='{"targets": []}'

# 5. A game with no catalog target is not gated and writes nothing.
root_none="$(gate_fixture gate-none "$NO_TARGET" 0)"
check "no target: write_target_env returns 0" "ok" \
  "$(gate "$root_none" 'ds_write_target_env minecraft-fabric' 2>/dev/null)"
check "no target: nothing was resolved" "" \
  "$(grep -c 'targets resolve' "$root_none/argv.log" 2>/dev/null | grep -v '^0$' || true)"
check "no target: preflight returns 0" "ok" \
  "$(gate "$root_none" 'ds_preflight_target minecraft-fabric' 2>/dev/null)"

# 6. ds_write_target_env asks the tool for exactly the env file the rig reads.
root_write="$(gate_fixture gate-write "$TARGET_LISTING" 0)"
check "write_target_env resolves the target" "ok" \
  "$(gate "$root_write" 'ds_write_target_env minecraft-fabric' 2>/dev/null)"
resolve_argv="$(grep 'targets resolve' "$root_write/argv.log")"
for want in "--game minecraft" "--target fabric-26.2" "--format env" "--prefix MC_FABRIC" \
            "--out ${root_write}/_data/.targets/minecraft-fabric.env"; do
  case "$resolve_argv" in
    *"$want"*) echo "PASS resolve argv carries '$want'" ;;
    *) echo "FAIL resolve argv is missing '$want': $resolve_argv" >&2; exit 1 ;;
  esac
done

# 7. A target-driven game with no resolved environment cannot boot, and the message says
#    which script writes it.
root_gate="$(gate_fixture gate-noenv "$TARGET_LISTING" 0)"
check "preflight without an env file dies" "died:1" \
  "$(gate "$root_gate" 'ds_preflight_target minecraft-fabric' 2>/dev/null)"
stderr="$(gate "$root_gate" 'ds_preflight_target minecraft-fabric' 2>&1 >/dev/null)"
case "$stderr" in
  *"install.sh minecraft-fabric"*) echo "PASS the missing-environment message names install.sh" ;;
  *) echo "FAIL the missing-environment message was unhelpful: $stderr" >&2; exit 1 ;;
esac

# 8. A ledger that does not hold the target stops the boot too.
root_stale="$(gate_fixture gate-stale "$TARGET_LISTING" 7)"
mkdir -p "$root_stale/_data/.targets"
: >"$root_stale/_data/.targets/minecraft-fabric.env"
check "preflight with a failing ledger check dies" "died:1" \
  "$(gate "$root_stale" 'ds_preflight_target minecraft-fabric' 2>/dev/null)"
stderr="$(gate "$root_stale" 'ds_preflight_target minecraft-fabric' 2>&1 >/dev/null)"
case "$stderr" in
  *"does not hold catalog target fabric-26.2"*) echo "PASS the stale-install message names the target" ;;
  *) echo "FAIL the stale-install message was unhelpful: $stderr" >&2; exit 1 ;;
esac

# 9. Env file present and the ledger holding the target: the rig may boot.
root_ok="$(gate_fixture gate-ok "$TARGET_LISTING" 0)"
mkdir -p "$root_ok/_data/.targets"
: >"$root_ok/_data/.targets/minecraft-fabric.env"
check "preflight passes on a held target" "ok" \
  "$(gate "$root_ok" 'ds_preflight_target minecraft-fabric' 2>/dev/null)"

echo "ALL PASS"
