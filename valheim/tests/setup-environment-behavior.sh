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
  local install_dir="${1:-$STUB_SERVER_DIR}"
  local managed_dir="$install_dir/valheim_server_Data/Managed"
  local assembly
  mkdir -p "$managed_dir"
  for assembly in "${REQUIRED_ASSEMBLIES[@]}"; do
    create_managed_assembly_fixture "$managed_dir/$assembly"
  done
}

create_managed_assembly_fixture() {
  local output="$1"
  mkdir -p "$(dirname "$output")"
  /bin/cp "$MANAGED_ASSEMBLY_FIXTURE" "$output"
}

create_fake_marker_assembly_fixture() {
  local output="$1"
  mkdir -p "$(dirname "$output")"
  printf 'MZ%0126dBSJB' 0 > "$output"
}

steamcmd_stub() {
  local platform="unknown"
  local previous=""
  local argument
  local count=0
  local install_dir="$STUB_SERVER_DIR"

  for argument in "$@"; do
    if [ "$previous" = "+@sSteamCmdForcePlatformType" ]; then
      platform="$argument"
    fi
    if [ "$previous" = "+force_install_dir" ]; then
      install_dir="$argument"
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
    first_success|empty_bepinex|corrupt_bepinex|fake_marker_bepinex|file_unavailable|valheim_publish_failure|valheim_publish_interrupt)
      create_required_assemblies "$install_dir"
      return 0
      ;;
    retry_success)
      if [ "$count" -eq 1 ]; then
        return 31
      fi
      create_required_assemblies "$install_dir"
      return 0
      ;;
    windows_success)
      if [ "$platform" = "linux" ]; then
        return 32
      fi
      create_required_assemblies "$install_dir"
      return 0
      ;;
    missing_required)
      mkdir -p "$install_dir/valheim_server_Data/Managed"
      create_managed_assembly_fixture "$install_dir/valheim_server_Data/Managed/assembly_valheim.dll"
      return 0
      ;;
    corrupt_required)
      local corrupt_dir="$install_dir/valheim_server_Data/Managed"
      local corrupt_assembly
      mkdir -p "$corrupt_dir"
      for corrupt_assembly in "${REQUIRED_ASSEMBLIES[@]}"; do
        printf 'not a managed assembly\n' > "$corrupt_dir/$corrupt_assembly"
      done
      return 0
      ;;
    empty_required)
      local managed_dir="$install_dir/valheim_server_Data/Managed"
      local assembly
      mkdir -p "$managed_dir"
      for assembly in "${REQUIRED_ASSEMBLIES[@]}"; do
        : > "$managed_dir/$assembly"
      done
      return 0
      ;;
    fake_marker_required)
      local marker_dir="$install_dir/valheim_server_Data/Managed"
      local marker_assembly
      mkdir -p "$marker_dir"
      for marker_assembly in "${REQUIRED_ASSEMBLIES[@]}"; do
        create_fake_marker_assembly_fixture "$marker_dir/$marker_assembly"
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
  local is_steamcmd_download=false
  local previous=""
  local argument
  for argument in "$@"; do
    if [ "$previous" = "-o" ]; then
      output="$argument"
    fi
    if [[ "$argument" == *steamcmd_linux.tar.gz ]]; then
      is_steamcmd_download=true
    fi
    previous="$argument"
  done

  if [ "$is_steamcmd_download" = true ]; then
    if [ -n "$output" ]; then
      mkdir -p "$(dirname "$output")"
      printf 'PARTIAL_RETRY_BYTES' > "$output"
      if [ "$STUB_SCENARIO" = "steamcmd_download_failure" ]; then
        return 66
      fi
      printf 'COMPLETE_ARCHIVE' > "$output"
      printf '%s\n' "$output" > "$STUB_STATE_DIR/curl-output"
      return 0
    fi

    # A retried stream can contain bytes from the failed transfer followed by the
    # completed transfer. Piping this directly to tar must fail the regression.
    printf 'PARTIAL_RETRY_BYTESCOMPLETE_ARCHIVE'
    return 0
  fi

  previous=""
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

tar_stub() {
  local archive=""
  local destination=""
  local previous=""
  local argument
  local archive_contents

  for argument in "$@"; do
    if [ "$previous" = "-xzf" ]; then
      archive="$argument"
    elif [ "$previous" = "-C" ]; then
      destination="$argument"
    fi
    previous="$argument"
  done

  if [ -z "$archive" ] || [ "$archive" = "-" ]; then
    archive_contents="$(cat)"
  else
    archive_contents="$(cat "$archive")"
  fi
  printf '%s' "$archive_contents" > "$STUB_STATE_DIR/tar-input"

  if [ "$archive_contents" != "COMPLETE_ARCHIVE" ]; then
    printf 'tar received a partial or concatenated SteamCMD archive\n' >&2
    return 67
  fi

  mkdir -p "$destination"
  ln -sf "$SELF" "$destination/steamcmd.sh"
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
  elif [ "$STUB_SCENARIO" = "corrupt_bepinex" ]; then
    printf 'not a managed assembly\n' > "$destination/BepInExPack_Valheim/BepInEx/core/BepInEx.dll"
    printf 'not a managed assembly\n' > "$destination/BepInExPack_Valheim/BepInEx/core/0Harmony.dll"
  elif [ "$STUB_SCENARIO" = "fake_marker_bepinex" ]; then
    create_fake_marker_assembly_fixture "$destination/BepInExPack_Valheim/BepInEx/core/BepInEx.dll"
    create_fake_marker_assembly_fixture "$destination/BepInExPack_Valheim/BepInEx/core/0Harmony.dll"
  else
    create_managed_assembly_fixture "$destination/BepInExPack_Valheim/BepInEx/core/BepInEx.dll"
    create_managed_assembly_fixture "$destination/BepInExPack_Valheim/BepInEx/core/0Harmony.dll"
  fi
}

cp_stub() {
  if [ "$STUB_SCENARIO" = "steamcmd_publish_failure" ]; then
    printf 'simulated SteamCMD publish failure\n' >&2
    return 68
  fi
  /bin/cp "$@"
}

mv_stub() {
  if [ "$STUB_SCENARIO" = "valheim_publish_interrupt" ] \
    && [[ "${1:-}" == "$STUB_SERVER_DIR".stage.* ]] \
    && [ "${2:-}" = "$STUB_SERVER_DIR" ]; then
    : > "$STUB_STATE_DIR/publish-interrupt-attempted"
    kill -TERM "$PPID"
    /bin/sleep 0.2
    return 70
  fi
  if [ "$STUB_SCENARIO" = "valheim_publish_failure" ] \
    && [[ "${1:-}" == "$STUB_SERVER_DIR".stage.* ]] \
    && [ "${2:-}" = "$STUB_SERVER_DIR" ]; then
    printf 'simulated atomic Valheim publication failure\n' >&2
    return 69
  fi
  /bin/mv "$@"
}

file_stub() {
  if [ "$STUB_SCENARIO" = "file_unavailable" ]; then
    printf "file: command unavailable\n" >&2
    return 127
  fi
  /usr/bin/file "$@"
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
  tar)
    tar_stub "$@"
    exit $?
    ;;
  cp)
    cp_stub "$@"
    exit $?
    ;;
  mv)
    mv_stub "$@"
    exit $?
    ;;
  file)
    file_stub "$@"
    exit $?
    ;;
  sleep)
    exit 0
    ;;
esac

VALHEIM_DIR="$(cd "$(dirname "$SELF")/.." && pwd)"
SETUP_SCRIPT="$VALHEIM_DIR/scripts/setup-environment.sh"
MANAGED_ASSEMBLY_FIXTURE="${MANAGED_ASSEMBLY_FIXTURE:-$VALHEIM_DIR/src/Takaro.Valheim.Core/bin/Debug/net8.0/Takaro.Valheim.Core.dll}"
if [ ! -f "$MANAGED_ASSEMBLY_FIXTURE" ]; then
  printf 'Managed assembly fixture is missing: %s\n' "$MANAGED_ASSEMBLY_FIXTURE" >&2
  exit 1
fi
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TMP_ROOT"' EXIT

RUN_STATUS=0
RUN_OUTPUT=""
RUN_CASE_DIR=""

run_setup() {
  local name="$1"
  local scenario="$2"
  local platforms="${3:-linux windows}"
  local preinstall_steamcmd="${4:-true}"
  local case_dir="$TMP_ROOT/$name"
  local bin_dir="$case_dir/bin"
  local command
  local steamcmd_path

  mkdir -p "$bin_dir" "$case_dir/home" "$case_dir/data" "$case_dir/server" "$case_dir/steamcmd" "$case_dir/deps" "$case_dir/state"
  for command in curl jq unzip tar cp mv file sleep; do
    ln -sf "$SELF" "$bin_dir/$command"
  done
  if [ "$preinstall_steamcmd" = true ]; then
    ln -sf "$SELF" "$bin_dir/steamcmd.sh"
    steamcmd_path="$bin_dir/steamcmd.sh"
  else
    steamcmd_path="$case_dir/steamcmd/steamcmd.sh"
  fi

  RUN_OUTPUT="$case_dir/output.log"
  RUN_CASE_DIR="$case_dir"
  env \
    PATH="$bin_dir:$PATH" \
    HOME="$case_dir/home" \
    VALHEIM_DATA_DIR="$case_dir/data" \
    STEAMCMD_DIR="$case_dir/steamcmd" \
    VALHEIM_SERVER_DIR="$case_dir/server" \
    VALHEIM_DEPS_DIR="$case_dir/deps" \
    STEAMCMD="$steamcmd_path" \
    VALHEIM_STEAM_PLATFORMS="$platforms" \
    STUB_SCENARIO="$scenario" \
    STUB_STATE_DIR="$case_dir/state" \
    STUB_SERVER_DIR="$case_dir/server" \
    STUB_STEAMCMD_DIR="$case_dir/steamcmd" \
    MANAGED_ASSEMBLY_FIXTURE="$MANAGED_ASSEMBLY_FIXTURE" \
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
    printf 'stale-valid-looking-reference\n' > "$case_dir/server/valheim_server_Data/Managed/$assembly"
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

test_corrupt_required_dlls_exhaust_all_attempts() {
  run_setup corrupt-required corrupt_required
  assert_nonzero "$RUN_STATUS" "non-managed Valheim DLL text must not satisfy reference validation" || return 1
  assert_equals 6 "$(call_count)" "corrupt DLLs should retry and fall back" || return 1
  assert_output_contains "managed PE/CLI assembly" "Valheim corruption failure should be actionable" || return 1
}

test_fake_managed_markers_do_not_satisfy_real_assembly_validation() {
  run_setup fake-marker-required fake_marker_required
  assert_nonzero "$RUN_STATUS" "MZ and BSJB marker placement must not satisfy managed assembly validation" || return 1
  assert_equals 6 "$(call_count)" "fake marker DLLs should retry and fall back" || return 1
}

test_empty_bepinex_dlls_fail_setup() {
  run_setup empty-bepinex empty_bepinex
  assert_nonzero "$RUN_STATUS" "zero-byte BepInEx DLLs must not satisfy reference validation" || return 1
  assert_output_contains "required BepInEx assembly" "BepInEx failure should name the missing or empty reference" || return 1
}

test_corrupt_bepinex_dlls_fail_setup() {
  run_setup corrupt-bepinex corrupt_bepinex
  assert_nonzero "$RUN_STATUS" "non-managed BepInEx DLL text must not satisfy reference validation" || return 1
  assert_output_contains "managed PE/CLI assembly" "BepInEx corruption failure should be actionable" || return 1
}

test_fake_bepinex_markers_do_not_satisfy_real_assembly_validation() {
  run_setup fake-marker-bepinex fake_marker_bepinex
  assert_nonzero "$RUN_STATUS" "fake BepInEx marker blobs must fail managed assembly validation" || return 1
}

test_failed_file_validator_is_actionable() {
  run_setup file-unavailable file_unavailable
  assert_nonzero "$RUN_STATUS" "setup must fail when its real PE/CLI validator is unavailable" || return 1
  assert_output_contains "managed PE/CLI assembly" "failed validator should identify the required assembly contract" || return 1
}

test_valid_existing_install_skips_steamcmd() {
  local case_dir="$TMP_ROOT/valid-existing"
  STUB_SERVER_DIR="$case_dir/server" create_required_assemblies "$case_dir/server"

  run_setup valid-existing always_fail
  assert_equals 0 "$RUN_STATUS" "a fully validated existing install should be reused" || return 1
  assert_equals 0 "$(call_count)" "validated existing references should skip SteamCMD" || return 1
}

test_failed_atomic_publication_rolls_back_and_next_run_retries() {
  local case_dir="$TMP_ROOT/atomic-publication"
  mkdir -p "$case_dir/server/worlds_local"
  printf '%s\n' "old install" > "$case_dir/server/worlds_local/world.db"

  run_setup atomic-publication valheim_publish_failure
  assert_nonzero "$RUN_STATUS" "a failed atomic publication must fail setup" || return 1
  assert_file "$RUN_CASE_DIR/server/worlds_local/world.db" "failed publication must restore the old install" || return 1
  assert_equals "old install" "$(cat "$RUN_CASE_DIR/server/worlds_local/world.db")" "rollback must retain old install content" || return 1

  run_setup atomic-publication first_success
  assert_equals 0 "$RUN_STATUS" "the next setup run must retry after failed publication" || return 1
  assert_nonempty_file "$RUN_CASE_DIR/server/valheim_server_Data/Managed/assembly_valheim.dll" "retry should publish validated references" || return 1
  assert_file "$RUN_CASE_DIR/server/worlds_local/world.db" "successful swap must preserve caller server data" || return 1
}

test_signal_during_atomic_publication_restores_old_install() {
  local case_dir="$TMP_ROOT/atomic-interrupt"
  mkdir -p "$case_dir/server/worlds_local"
  printf '%s\n' "old install" > "$case_dir/server/worlds_local/world.db"

  run_setup atomic-interrupt valheim_publish_interrupt
  assert_nonzero "$RUN_STATUS" "an interrupted atomic publication must fail setup" || return 1
  assert_file "$RUN_CASE_DIR/state/publish-interrupt-attempted" "test must reach the atomic publication boundary" || return 1
  assert_file "$RUN_CASE_DIR/server/worlds_local/world.db" "signal cleanup must restore the old install" || return 1
  assert_equals "old install" "$(cat "$RUN_CASE_DIR/server/worlds_local/world.db")" "signal rollback must retain old install content" || return 1
}

test_steamcmd_download_is_completed_before_tar_reads_it() {
  run_setup steamcmd-download first_success "linux windows" false
  assert_equals 0 "$RUN_STATUS" "completed SteamCMD archive should install and run" || return 1
  assert_file "$RUN_CASE_DIR/state/tar-input" "tar should receive an archive file" || return 1
  assert_equals "COMPLETE_ARCHIVE" "$(cat "$RUN_CASE_DIR/state/tar-input")" "tar must see only the completed retry result" || return 1
  if find "$RUN_CASE_DIR/steamcmd" -maxdepth 1 -name 'steamcmd.*.tar.gz' -print -quit | grep -q .; then
    printf 'ASSERT: successful SteamCMD archive temporary file was not cleaned up\n' >&2
    return 1
  fi
  if find "$RUN_CASE_DIR/steamcmd" -maxdepth 1 -type d -name 'steamcmd-extract.*' -print -quit | grep -q .; then
    printf 'ASSERT: successful SteamCMD extraction directory was not cleaned up\n' >&2
    return 1
  fi
}

test_failed_steamcmd_download_cleans_partial_archive() {
  run_setup steamcmd-download-failure steamcmd_download_failure "linux windows" false
  assert_nonzero "$RUN_STATUS" "failed SteamCMD download must fail setup before extraction" || return 1
  if [ -f "$RUN_CASE_DIR/state/tar-input" ]; then
    printf 'ASSERT: tar must not inspect a failed SteamCMD download\n' >&2
    return 1
  fi
  if find "$RUN_CASE_DIR/steamcmd" -maxdepth 1 -name 'steamcmd.*.tar.gz' -print -quit | grep -q .; then
    printf 'ASSERT: failed SteamCMD archive temporary file was not cleaned up\n' >&2
    return 1
  fi
}

test_failed_steamcmd_publish_cleans_archive_and_extract_directory() {
  run_setup steamcmd-publish-failure steamcmd_publish_failure "linux windows" false
  assert_nonzero "$RUN_STATUS" "failed SteamCMD publish must fail setup" || return 1
  if find "$RUN_CASE_DIR/steamcmd" -maxdepth 1 \( -name 'steamcmd.*.tar.gz' -o -name 'steamcmd-extract.*' \) -print -quit | grep -q .; then
    printf 'ASSERT: failed SteamCMD publish left temporary archive or extraction state\n' >&2
    return 1
  fi
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
  test_corrupt_required_dlls_exhaust_all_attempts \
  test_fake_managed_markers_do_not_satisfy_real_assembly_validation \
  test_empty_bepinex_dlls_fail_setup \
  test_corrupt_bepinex_dlls_fail_setup \
  test_fake_bepinex_markers_do_not_satisfy_real_assembly_validation \
  test_failed_file_validator_is_actionable \
  test_valid_existing_install_skips_steamcmd \
  test_failed_atomic_publication_rolls_back_and_next_run_retries \
  test_signal_during_atomic_publication_restores_old_install \
  test_steamcmd_download_is_completed_before_tar_reads_it \
  test_failed_steamcmd_download_cleans_partial_archive \
  test_failed_steamcmd_publish_cleans_archive_and_extract_directory \
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
