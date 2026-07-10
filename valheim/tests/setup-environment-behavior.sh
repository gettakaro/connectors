#!/usr/bin/env bash
set -uo pipefail

SELF="$(realpath "${BASH_SOURCE[0]}")"
COMMAND_NAME="$(basename "$0")"
REQUIRED_ASSEMBLIES=(
  assembly_valheim.dll
  assembly_utils.dll
  Splatform.dll
  UnityEngine.dll
  UnityEngine.CoreModule.dll
)

create_required_assemblies() {
  local managed_dir="$STUB_SERVER_DIR/valheim_server_Data/Managed"
  local assembly
  mkdir -p "$managed_dir"
  for assembly in "${REQUIRED_ASSEMBLIES[@]}"; do
    printf 'test-reference\n' > "$managed_dir/$assembly"
  done
}

steamcmd_stub() {
  local platform="unknown"
  local previous=""
  local argument
  local count=0

  for argument in "$@"; do
    if [ "$previous" = "+@sSteamCmdForcePlatformType" ]; then
      platform="$argument"
      break
    fi
    previous="$argument"
  done

  mkdir -p "$STUB_STATE_DIR"
  if [ -f "$STUB_STATE_DIR/count" ]; then
    read -r count < "$STUB_STATE_DIR/count"
  fi
  count=$((count + 1))
  printf '%s\n' "$count" > "$STUB_STATE_DIR/count"
  printf '%s\n' "$platform" >> "$STUB_STATE_DIR/platforms"

  if [ "$count" -gt 1 ] \
    && { [ -e "$HOME/Steam/appcache/sentinel" ] || [ -e "$STUB_STEAMCMD_DIR/appcache/sentinel" ]; }; then
    printf '%s\n' "cache not cleared before attempt $count" >> "$STUB_STATE_DIR/errors"
    return 88
  fi

  case "$STUB_SCENARIO" in
    first_success|empty_bepinex)
      create_required_assemblies
      return 0
      ;;
    retry_success)
      if [ "$count" -eq 1 ]; then
        return 31
      fi
      create_required_assemblies
      return 0
      ;;
    windows_success)
      if [ "$platform" = "linux" ]; then
        return 32
      fi
      create_required_assemblies
      return 0
      ;;
    missing_required)
      mkdir -p "$STUB_SERVER_DIR/valheim_server_Data/Managed"
      printf 'test-reference\n' > "$STUB_SERVER_DIR/valheim_server_Data/Managed/assembly_valheim.dll"
      return 0
      ;;
    empty_required)
      local managed_dir="$STUB_SERVER_DIR/valheim_server_Data/Managed"
      local assembly
      mkdir -p "$managed_dir"
      for assembly in "${REQUIRED_ASSEMBLIES[@]}"; do
        : > "$managed_dir/$assembly"
      done
      return 0
      ;;
    always_fail)
      return 33
      ;;
    *)
      printf 'unknown SteamCMD test scenario: %s\n' "$STUB_SCENARIO" >&2
      return 99
      ;;
  esac
}

curl_stub() {
  local output=""
  while [ "$#" -gt 0 ]; do
    if [ "$1" = "-o" ]; then
      shift
      output="${1:-}"
      break
    fi
    shift
  done

  if [ -n "$output" ]; then
    mkdir -p "$(dirname "$output")"
    : > "$output"
  else
    printf '%s\n' '{"latest":{"download_url":"https://example.invalid/bepinex.zip"}}'
  fi
}

jq_stub() {
  while IFS= read -r _; do
    :
  done
  printf '%s\n' 'https://example.invalid/bepinex.zip'
}

unzip_stub() {
  local destination=""
  while [ "$#" -gt 0 ]; do
    if [ "$1" = "-d" ]; then
      shift
      destination="${1:-}"
      break
    fi
    shift
  done

  mkdir -p "$destination/BepInExPack_Valheim/BepInEx/core"
  if [ "$STUB_SCENARIO" = "empty_bepinex" ]; then
    : > "$destination/BepInExPack_Valheim/BepInEx/core/BepInEx.dll"
    : > "$destination/BepInExPack_Valheim/BepInEx/core/0Harmony.dll"
  else
    printf 'test-reference\n' > "$destination/BepInExPack_Valheim/BepInEx/core/BepInEx.dll"
    printf 'test-reference\n' > "$destination/BepInExPack_Valheim/BepInEx/core/0Harmony.dll"
  fi
}

case "$COMMAND_NAME" in
  steamcmd.sh)
    steamcmd_stub "$@"
    exit $?
    ;;
  curl)
    curl_stub "$@"
    exit $?
    ;;
  jq)
    jq_stub "$@"
    exit $?
    ;;
  unzip)
    unzip_stub "$@"
    exit $?
    ;;
  sleep)
    exit 0
    ;;
esac

VALHEIM_DIR="$(cd "$(dirname "$SELF")/.." && pwd)"
SETUP_SCRIPT="$VALHEIM_DIR/scripts/setup-environment.sh"
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TMP_ROOT"' EXIT

RUN_STATUS=0
RUN_OUTPUT=""
RUN_CASE_DIR=""

run_setup() {
  local name="$1"
  local scenario="$2"
  local platforms="${3:-linux windows}"
  local case_dir="$TMP_ROOT/$name"
  local bin_dir="$case_dir/bin"
  local command

  mkdir -p "$bin_dir" "$case_dir/home" "$case_dir/data" "$case_dir/server" "$case_dir/steamcmd" "$case_dir/deps" "$case_dir/state"
  for command in steamcmd.sh curl jq unzip sleep; do
    ln -s "$SELF" "$bin_dir/$command"
  done

  RUN_OUTPUT="$case_dir/output.log"
  RUN_CASE_DIR="$case_dir"
  env \
    PATH="$bin_dir:$PATH" \
    HOME="$case_dir/home" \
    VALHEIM_DATA_DIR="$case_dir/data" \
    STEAMCMD_DIR="$case_dir/steamcmd" \
    VALHEIM_SERVER_DIR="$case_dir/server" \
    VALHEIM_DEPS_DIR="$case_dir/deps" \
    STEAMCMD="$bin_dir/steamcmd.sh" \
    VALHEIM_STEAM_PLATFORMS="$platforms" \
    STUB_SCENARIO="$scenario" \
    STUB_STATE_DIR="$case_dir/state" \
    STUB_SERVER_DIR="$case_dir/server" \
    STUB_STEAMCMD_DIR="$case_dir/steamcmd" \
    bash "$SETUP_SCRIPT" > "$RUN_OUTPUT" 2>&1
  RUN_STATUS=$?
}

call_count() {
  if [ ! -f "$RUN_CASE_DIR/state/platforms" ]; then
    printf '0\n'
    return
  fi
  wc -l < "$RUN_CASE_DIR/state/platforms"
}

assert_equals() {
  local expected="$1"
  local actual="$2"
  local message="$3"
  if [ "$expected" != "$actual" ]; then
    printf 'ASSERT: %s (expected=%s actual=%s)\n' "$message" "$expected" "$actual" >&2
    return 1
  fi
}

assert_nonzero() {
  local actual="$1"
  local message="$2"
  if [ "$actual" -eq 0 ]; then
    printf 'ASSERT: %s (actual exit=0)\n' "$message" >&2
    return 1
  fi
}

assert_file() {
  local path="$1"
  local message="$2"
  if [ ! -f "$path" ]; then
    printf 'ASSERT: %s (missing %s)\n' "$message" "$path" >&2
    return 1
  fi
}

assert_nonempty_file() {
  local path="$1"
  local message="$2"
  if [ ! -s "$path" ]; then
    printf 'ASSERT: %s (missing or empty %s)\n' "$message" "$path" >&2
    return 1
  fi
}

assert_output_contains() {
  local needle="$1"
  local message="$2"
  if ! grep -Fq "$needle" "$RUN_OUTPUT"; then
    printf 'ASSERT: %s (missing output: %s)\n' "$message" "$needle" >&2
    return 1
  fi
}

test_first_attempt_success() {
  run_setup first-success first_success
  assert_equals 0 "$RUN_STATUS" "first successful SteamCMD run should complete setup" || return 1
  assert_equals 1 "$(call_count)" "success should not retry" || return 1
  assert_nonempty_file "$RUN_CASE_DIR/server/valheim_server_Data/Managed/UnityEngine.CoreModule.dll" "required assemblies should be nonempty" || return 1
  assert_nonempty_file "$RUN_CASE_DIR/deps/bepinex/BepInExPack_Valheim/BepInEx/core/BepInEx.dll" "BepInEx reference should be nonempty" || return 1
}

test_retry_recovery_clears_cache() {
  local case_dir="$TMP_ROOT/retry-recovery"
  mkdir -p "$case_dir/home/Steam/appcache" "$case_dir/steamcmd/appcache"
  : > "$case_dir/home/Steam/appcache/sentinel"
  : > "$case_dir/steamcmd/appcache/sentinel"

  run_setup retry-recovery retry_success
  assert_equals 0 "$RUN_STATUS" "second SteamCMD attempt should recover" || return 1
  assert_equals 2 "$(call_count)" "recovery should use exactly two attempts" || return 1
  if [ -f "$RUN_CASE_DIR/state/errors" ]; then
    printf 'ASSERT: retry observed uncleared cache\n' >&2
    return 1
  fi
}

test_failed_steamcmd_does_not_accept_stale_managed_directory() {
  local case_dir="$TMP_ROOT/stale-managed"
  local assembly
  mkdir -p "$case_dir/server/valheim_server_Data/Managed"
  for assembly in "${REQUIRED_ASSEMBLIES[@]}"; do
    : > "$case_dir/server/valheim_server_Data/Managed/$assembly"
  done

  run_setup stale-managed always_fail
  assert_nonzero "$RUN_STATUS" "failed SteamCMD must not accept stale assemblies" || return 1
  assert_equals 6 "$(call_count)" "stale cache must not bypass retries or platform fallback" || return 1
}

test_linux_windows_fallback_preserves_caller_server_data() {
  local case_dir="$TMP_ROOT/platform-fallback"
  mkdir -p "$case_dir/server/worlds_local"
  printf '%s\n' "keep me" > "$case_dir/server/worlds_local/world.db"

  run_setup platform-fallback windows_success
  assert_equals 0 "$RUN_STATUS" "Windows compile-reference fallback should succeed" || return 1
  assert_equals 4 "$(call_count)" "fallback should exhaust Linux then try Windows once" || return 1
  assert_file "$RUN_CASE_DIR/server/worlds_local/world.db" "platform fallback must not delete caller-controlled server data" || return 1
  assert_output_contains "Windows depot for compile references only" "fallback purpose should be explicit" || return 1
}

test_missing_required_dlls_exhausts_all_attempts() {
  run_setup missing-required missing_required
  assert_nonzero "$RUN_STATUS" "a Managed directory without every required DLL must fail" || return 1
  assert_equals 6 "$(call_count)" "missing DLLs should retry and fall back" || return 1
}

test_empty_required_dlls_exhaust_all_attempts() {
  run_setup empty-required empty_required
  assert_nonzero "$RUN_STATUS" "zero-byte Valheim DLLs must not satisfy reference validation" || return 1
  assert_equals 6 "$(call_count)" "empty DLLs should retry and fall back" || return 1
}

test_empty_bepinex_dlls_fail_setup() {
  run_setup empty-bepinex empty_bepinex
  assert_nonzero "$RUN_STATUS" "zero-byte BepInEx DLLs must not satisfy reference validation" || return 1
  assert_output_contains "required BepInEx assembly" "BepInEx failure should name the missing or empty reference" || return 1
}

test_exhaustion_reports_recovery_context() {
  run_setup exhausted always_fail
  assert_nonzero "$RUN_STATUS" "exhausted SteamCMD attempts should fail" || return 1
  assert_equals 6 "$(call_count)" "three attempts per configured platform should run" || return 1
  assert_output_contains "$RUN_CASE_DIR/server/valheim_server_Data/Managed" "failure should print expected Managed path" || return 1
  assert_output_contains "Attempted Steam platforms: linux windows" "failure should print attempted platforms" || return 1
  assert_output_contains "VALHEIM_STEAM_PLATFORMS" "failure should print platform recovery context" || return 1
}

failures=0
for test_case in \
  test_first_attempt_success \
  test_retry_recovery_clears_cache \
  test_failed_steamcmd_does_not_accept_stale_managed_directory \
  test_linux_windows_fallback_preserves_caller_server_data \
  test_missing_required_dlls_exhausts_all_attempts \
  test_empty_required_dlls_exhaust_all_attempts \
  test_empty_bepinex_dlls_fail_setup \
  test_exhaustion_reports_recovery_context; do
  if "$test_case"; then
    printf 'PASS %s\n' "$test_case"
  else
    printf 'FAIL %s\n' "$test_case"
    failures=$((failures + 1))
  fi
done

if [ "$failures" -ne 0 ]; then
  printf '%s setup behavior test(s) failed\n' "$failures" >&2
  exit 1
fi

printf 'All setup behavior tests passed\n'
