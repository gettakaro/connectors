#!/usr/bin/env bash
# The rig's Dune database migration (dev-servers/lib/games/dune.sh, dune_migrate_db_name) against a
# fake `docker exec … psql` whose databases are files: <name> holds its character count.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
FAILURES=0

mkdir -p "$WORK/bin"
cat > "$WORK/bin/docker" <<'FAKE'
#!/usr/bin/env bash
# docker exec takaro-dev-dune-postgres psql -U dune -d <db> -Atc <sql>
db="$7"; sql="$9"; dir="$FAKE_PG"
case "$sql" in
  "select datname from pg_database where datname like"*)
    keep="$(sed -n "s/.*datname <> '\([^']*\)'.*/\1/p" <<<"$sql")"
    for f in "$dir"/dune_sb_*; do [ -e "$f" ] && [ "${f##*/}" != "$keep" ] && echo "${f##*/}"; done ;;
  "select 1 from pg_database where datname="*)
    name="$(sed -n "s/.*datname='\([^']*\)'.*/\1/p" <<<"$sql")"
    [ -e "$dir/$name" ] && echo 1 ;;
  "select count(*) from dune.player_state")
    cat "$dir/$db" ;;
  "ALTER DATABASE"*)
    from="$(sed -n 's/^ALTER DATABASE "\([^"]*\)".*/\1/p' <<<"$sql")"
    to="$(sed -n 's/.*RENAME TO "\([^"]*\)".*/\1/p' <<<"$sql")"
    mv "$dir/$from" "$dir/$to" ;;
  *) echo "fake psql: unexpected $sql" >&2; exit 1 ;;
esac
FAKE
chmod +x "$WORK/bin/docker"

# Each case runs in a subshell so ds_die's exit ends only that case.
run_case() {
    local name="$1" expect_status="$2"; shift 2
    local dir="$WORK/$name"; mkdir -p "$dir"; CASE_DIR="$dir"
    local spec
    for spec in "$@"; do printf '%s' "${spec#*=}" > "$dir/${spec%%=*}"; done
    # The stubs below are called by the sourced library, which shellcheck cannot see.
    # shellcheck disable=SC2317,SC2329,SC2034
    (
        export PATH="$WORK/bin:$PATH" FAKE_PG="$dir" REPO_ROOT="$ROOT" DS_DATA="$WORK/data"
        ds_register() { :; }
        ds_info() { :; }
        ds_ok() { :; }
        ds_die() { echo "DIE: $*" >&2; exit 3; }
        ds_compose() { :; }
        # shellcheck source=/dev/null
        source "$ROOT/dev-servers/lib/games/dune.sh"
        DUNE_DB_NAME=dune_sb_1_5_15_0
        dune_migrate_db_name
    ) 2> "$dir/stderr"
    local status=$?
    if [ "$status" -ne "$expect_status" ]; then
        echo "FAIL $name: exit $status, want $expect_status ($(cat "$dir/stderr"))"; FAILURES=$((FAILURES + 1))
    fi
}

expect_dbs() {
    local dir="$1" want="$2" got
    local f
    got=""
    for f in "$dir"/dune_sb_*; do [ -e "$f" ] && got+="${f##*/} "; done
    if [ "$got" != "$want" ]; then
        echo "FAIL ${dir##*/}: databases '$got', want '$want'"; FAILURES=$((FAILURES + 1))
    fi
}

run_case upgrade 0 dune_sb_1_5_3_0=4; d="$CASE_DIR"
expect_dbs "$d" "dune_sb_1_5_15_0 "
[ "$(cat "$d/dune_sb_1_5_15_0")" = 4 ] || { echo "FAIL upgrade: the world did not move"; FAILURES=$((FAILURES + 1)); }

run_case current 0 dune_sb_1_5_15_0=4; d="$CASE_DIR"
expect_dbs "$d" "dune_sb_1_5_15_0 "

run_case fresh 0; d="$CASE_DIR"
expect_dbs "$d" ""

run_case partial_upgrade 3 dune_sb_1_5_3_0=4 dune_sb_1_5_15_0=0; d="$CASE_DIR"
expect_dbs "$d" "dune_sb_1_5_15_0 dune_sb_1_5_3_0 "
grep -q "drop the empty dune_sb_1_5_15_0" "$d/stderr" || { echo "FAIL partial_upgrade: no guidance"; FAILURES=$((FAILURES + 1)); }

run_case upgraded_old_kept 0 dune_sb_1_5_3_0=4 dune_sb_1_5_15_0=6; d="$CASE_DIR"
expect_dbs "$d" "dune_sb_1_5_15_0 dune_sb_1_5_3_0 "

run_case ambiguous 3 dune_sb_1_4_0_0=2 dune_sb_1_5_3_0=4; d="$CASE_DIR"
expect_dbs "$d" "dune_sb_1_4_0_0 dune_sb_1_5_3_0 "

if [ "$FAILURES" -eq 0 ]; then echo "dune db migration: all cases passed"; else exit 1; fi
