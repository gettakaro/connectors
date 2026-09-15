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
REFERENCE_CACHE_MARKER_NAME=".takaro-valheim-reference-cache"
REFERENCE_CACHE_MARKER_CONTENT="takaro-valheim-reference-cache-v1"

mark_owned_reference_cache() {
  local cache_dir="$1"
  mkdir -p "$cache_dir"
  printf '%s\n' "$REFERENCE_CACHE_MARKER_CONTENT" > "$cache_dir/$REFERENCE_CACHE_MARKER_NAME"
}

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
    first_success|empty_bepinex|corrupt_bepinex|fake_marker_bepinex|file_unavailable|valheim_publish_failure|valheim_publish_interrupt|valheim_first_rename_interrupt)
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
    local download_count=0
    if [ -f "$STUB_STATE_DIR/steamcmd-download-count" ]; then
      read -r download_count < "$STUB_STATE_DIR/steamcmd-download-count"
    fi
    printf '%s\n' "$((download_count + 1))" > "$STUB_STATE_DIR/steamcmd-download-count"
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

  if [ "$STUB_SCENARIO" = "steamcmd_extract_failure" ]; then
    mkdir -p "$destination"
    printf 'partial extraction\n' > "$destination/partial-extraction"
    printf 'simulated SteamCMD extraction failure\n' >&2
    return 69
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

write_poisoned_steamcmd_destination() {
  local destination="$1"
  mkdir -p "$destination"
  # Expand STUB_STATE_DIR when the generated stub runs, not while writing it.
  # shellcheck disable=SC2016
  printf '%s\n' \
    '#!/usr/bin/env bash' \
    ': > "$STUB_STATE_DIR/poisoned-steamcmd-ran"' \
    'exit 91' > "$destination/steamcmd.sh"
  chmod +x "$destination/steamcmd.sh"
}

cp_stub() {
  local destination="${*: -1}"
  if [ "$STUB_SCENARIO" = "steamcmd_partial_publish_failure" ] \
    && [ "${destination%/}" = "$STUB_STEAMCMD_DIR" ]; then
    write_poisoned_steamcmd_destination "$STUB_STEAMCMD_DIR"
    printf 'simulated partial SteamCMD publish failure\n' >&2
    return 68
  fi
  if [ "$STUB_SCENARIO" = "steamcmd_partial_publish_interrupt" ] \
    && [ "${destination%/}" = "$STUB_STEAMCMD_DIR" ]; then
    write_poisoned_steamcmd_destination "$STUB_STEAMCMD_DIR"
    : > "$STUB_STATE_DIR/steamcmd-publish-interrupt-attempted"
    kill -TERM "$PPID"
    /bin/sleep 0.2
    return 72
  fi
  if [ "$STUB_SCENARIO" = "steamcmd_publish_failure" ]; then
    printf 'simulated SteamCMD publish failure\n' >&2
    return 68
  fi
  /bin/cp "$@"
}

mv_stub() {
  if [ "$STUB_SCENARIO" = "steamcmd_partial_publish_failure" ] \
    && [[ "${1:-}" == "$STUB_STEAMCMD_DIR".stage.* ]] \
    && [ "${2:-}" = "$STUB_STEAMCMD_DIR" ]; then
    write_poisoned_steamcmd_destination "$STUB_STEAMCMD_DIR"
    printf 'simulated partial atomic SteamCMD publication failure\n' >&2
    return 72
  fi
  if [ "$STUB_SCENARIO" = "steamcmd_partial_publish_interrupt" ] \
    && [[ "${1:-}" == "$STUB_STEAMCMD_DIR".stage.* ]] \
    && [ "${2:-}" = "$STUB_STEAMCMD_DIR" ]; then
    write_poisoned_steamcmd_destination "$STUB_STEAMCMD_DIR"
    : > "$STUB_STATE_DIR/steamcmd-publish-interrupt-attempted"
    kill -TERM "$PPID"
    /bin/sleep 0.2
    return 73
  fi
  if [ "$STUB_SCENARIO" = "valheim_first_rename_interrupt" ] \
    && [ "${1:-}" = "$STUB_SERVER_DIR" ] \
    && [[ "${2:-}" == "$STUB_SERVER_DIR".backup.* ]]; then
    /bin/mv "$@"
    : > "$STUB_STATE_DIR/first-rename-interrupt-attempted"
    kill -TERM "$PPID"
    /bin/sleep 0.2
    return 71
  fi
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
  local cache_variable="${5:-reference}"
  local case_dir="$TMP_ROOT/$name"
  local bin_dir="$case_dir/bin"
  local command
  local steamcmd_path
  local -a cache_environment

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
  case "$cache_variable" in
    reference)
      cache_environment=("VALHEIM_REFERENCE_CACHE_DIR=$case_dir/server")
      ;;
    legacy)
      cache_environment=("VALHEIM_SERVER_DIR=$case_dir/server")
      ;;
    *)
      printf 'unknown cache variable mode: %s\n' "$cache_variable" >&2
      return 2
      ;;
  esac
  env \
    PATH="$bin_dir:$PATH" \
    HOME="$case_dir/home" \
    VALHEIM_DATA_DIR="$case_dir/data" \
    STEAMCMD_DIR="$case_dir/steamcmd" \
    VALHEIM_DEPS_DIR="$case_dir/deps" \
    STEAMCMD="$steamcmd_path" \
    VALHEIM_STEAM_PLATFORMS="$platforms" \
    STUB_SCENARIO="$scenario" \
    STUB_STATE_DIR="$case_dir/state" \
    STUB_SERVER_DIR="$case_dir/server" \
    STUB_STEAMCMD_DIR="$case_dir/steamcmd" \
    MANAGED_ASSEMBLY_FIXTURE="$MANAGED_ASSEMBLY_FIXTURE" \
    "${cache_environment[@]}" \
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

assert_no_steamcmd_temporary_state() {
  local leaked_path
  leaked_path="$(find "$RUN_CASE_DIR" -mindepth 1 -maxdepth 1 \
    \( -name 'steamcmd.download.*' -o -name 'steamcmd.stage.*' -o -name 'steamcmd.backup.*' \) \
    -print -quit)"
  if [ -n "$leaked_path" ]; then
    printf 'ASSERT: SteamCMD sibling temporary state was not cleaned up: %s\n' "$leaked_path" >&2
    return 1
  fi
}

assert_no_server_temporary_state() {
  local leaked_path
  leaked_path="$(find "$RUN_CASE_DIR" -mindepth 1 -maxdepth 1 \
    \( -name 'server.stage.*' -o -name 'server.backup.*' \) \
    -print -quit)"
  if [ -n "$leaked_path" ]; then
    printf 'ASSERT: Valheim reference-cache sibling temporary state was not cleaned up: %s\n' "$leaked_path" >&2
    return 1
  fi
}

tree_fingerprint() {
  local directory="$1"
  (
    cd "$directory" || exit 1
    find . -type f -print0 \
      | sort -z \
      | while IFS= read -r -d '' path; do
          printf '%s\0' "$path"
          sha256sum -- "$path"
        done
  ) | sha256sum | awk '{print $1}'
}

test_first_attempt_success() {
  run_setup first-success first_success
  assert_equals 0 "$RUN_STATUS" "first successful SteamCMD run should complete setup" || return 1
  assert_equals 1 "$(call_count)" "success should not retry" || return 1
  assert_nonempty_file "$RUN_CASE_DIR/server/valheim_server_Data/Managed/UnityEngine.CoreModule.dll" "required assemblies should be nonempty" || return 1
  assert_file "$RUN_CASE_DIR/server/$REFERENCE_CACHE_MARKER_NAME" "a newly published reference cache must carry its ownership marker" || return 1
  assert_equals "$REFERENCE_CACHE_MARKER_CONTENT" "$(cat "$RUN_CASE_DIR/server/$REFERENCE_CACHE_MARKER_NAME")" "the ownership marker must be written only with the completed format" || return 1
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
  mark_owned_reference_cache "$case_dir/server"

  run_setup stale-managed always_fail
  assert_nonzero "$RUN_STATUS" "failed SteamCMD must not accept stale assemblies" || return 1
  assert_equals 6 "$(call_count)" "stale cache must not bypass retries or platform fallback" || return 1
}

test_owned_reference_cache_linux_windows_fallback_preserves_cache_data() {
  local case_dir="$TMP_ROOT/platform-fallback"
  mkdir -p "$case_dir/server/worlds_local"
  printf '%s\n' "keep me" > "$case_dir/server/worlds_local/world.db"
  mark_owned_reference_cache "$case_dir/server"

  run_setup platform-fallback windows_success
  assert_equals 0 "$RUN_STATUS" "Windows compile-reference fallback should succeed" || return 1
  assert_equals 4 "$(call_count)" "fallback should exhaust Linux then try Windows once" || return 1
  assert_file "$RUN_CASE_DIR/server/worlds_local/world.db" "platform fallback must preserve owned cache data" || return 1
  assert_file "$RUN_CASE_DIR/server/$REFERENCE_CACHE_MARKER_NAME" "successful fallback must publish a completed ownership marker" || return 1
  assert_output_contains "Windows depot for compile references only" "fallback purpose should be explicit" || return 1
}

test_invalid_legacy_live_server_is_refused_before_steamcmd_and_unchanged() {
  local case_dir="$TMP_ROOT/legacy-live-server"
  local before
  local after
  mkdir -p "$case_dir/server/worlds_local" "$case_dir/server/valheim_server_Data/Managed"
  printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$case_dir/server/valheim_server.x86_64"
  chmod +x "$case_dir/server/valheim_server.x86_64"
  printf '%s\n' 'irreplaceable world' > "$case_dir/server/worlds_local/world.db"
  printf '%s\n' 'invalid reference' > "$case_dir/server/valheim_server_Data/Managed/assembly_valheim.dll"
  before="$(tree_fingerprint "$case_dir/server")"

  run_setup legacy-live-server windows_success "linux windows" true legacy
  after="$(tree_fingerprint "$case_dir/server")"

  assert_nonzero "$RUN_STATUS" "an invalid unowned legacy live-server tree must be refused" || return 1
  assert_equals 0 "$(call_count)" "refusal must happen before SteamCMD can mutate a live server tree" || return 1
  assert_equals "$before" "$after" "every live-server file and hash must stay unchanged" || return 1
  assert_output_contains "VALHEIM_REFERENCE_CACHE_DIR" "refusal must direct the caller to a separate owned cache" || return 1
  assert_output_contains "valheim_server.x86_64" "refusal must identify the live-server marker" || return 1
  if [ -e "$RUN_CASE_DIR/server/$REFERENCE_CACHE_MARKER_NAME" ]; then
    printf 'ASSERT: refusal forged an ownership marker inside the live server\n' >&2
    return 1
  fi
  assert_no_server_temporary_state || return 1
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

test_missing_file_command_fails_preflight() {
  local case_dir="$TMP_ROOT/file-command-absent"
  local bin_dir="$case_dir/bin"
  mkdir -p "$bin_dir" "$case_dir/home"
  ln -sf /bin/bash "$bin_dir/bash"
  ln -sf /usr/bin/dirname "$bin_dir/dirname"

  if PATH="$bin_dir" command -v file >/dev/null 2>&1; then
    printf "ASSERT: isolated preflight PATH unexpectedly contains the 'file' command\n" >&2
    return 1
  fi

  RUN_OUTPUT="$case_dir/output.log"
  RUN_CASE_DIR="$case_dir"
  /usr/bin/env \
    PATH="$bin_dir" \
    HOME="$case_dir/home" \
    /bin/bash "$SETUP_SCRIPT" > "$RUN_OUTPUT" 2>&1
  RUN_STATUS=$?

  assert_nonzero "$RUN_STATUS" "setup must fail its preflight when the file command is truly absent" || return 1
  assert_output_contains "requires the 'file' command" "missing validator preflight should be actionable" || return 1
}

test_valid_existing_install_skips_steamcmd() {
  local case_dir="$TMP_ROOT/valid-existing"
  STUB_SERVER_DIR="$case_dir/server" create_required_assemblies "$case_dir/server"

  run_setup valid-existing always_fail "linux windows" true legacy
  assert_equals 0 "$RUN_STATUS" "a fully validated existing install should be reused" || return 1
  assert_equals 0 "$(call_count)" "validated existing references should skip SteamCMD" || return 1
  if [ -e "$RUN_CASE_DIR/server/$REFERENCE_CACHE_MARKER_NAME" ]; then
    printf 'ASSERT: read-only reuse must not claim ownership of a caller installation\n' >&2
    return 1
  fi
}

test_failed_atomic_publication_rolls_back_and_next_run_retries() {
  local case_dir="$TMP_ROOT/atomic-publication"
  mkdir -p "$case_dir/server/worlds_local"
  printf '%s\n' "old install" > "$case_dir/server/worlds_local/world.db"
  mark_owned_reference_cache "$case_dir/server"

  run_setup atomic-publication valheim_publish_failure
  assert_nonzero "$RUN_STATUS" "a failed atomic publication must fail setup" || return 1
  assert_file "$RUN_CASE_DIR/server/worlds_local/world.db" "failed publication must restore the old install" || return 1
  assert_equals "old install" "$(cat "$RUN_CASE_DIR/server/worlds_local/world.db")" "rollback must retain old install content" || return 1
  assert_no_server_temporary_state || return 1

  run_setup atomic-publication first_success
  assert_equals 0 "$RUN_STATUS" "the next setup run must retry after failed publication" || return 1
  assert_nonempty_file "$RUN_CASE_DIR/server/valheim_server_Data/Managed/assembly_valheim.dll" "retry should publish validated references" || return 1
  assert_file "$RUN_CASE_DIR/server/worlds_local/world.db" "successful swap must preserve caller server data" || return 1
  assert_no_server_temporary_state || return 1
}

test_failed_first_publication_does_not_forge_cache_ownership() {
  run_setup failed-first-publication valheim_publish_failure

  assert_nonzero "$RUN_STATUS" "failed first reference-cache publication must fail setup" || return 1
  if [ -e "$RUN_CASE_DIR/server/$REFERENCE_CACHE_MARKER_NAME" ]; then
    printf 'ASSERT: a partial publication forged a completed cache ownership marker\n' >&2
    return 1
  fi
  assert_no_server_temporary_state || return 1
}

test_signal_during_atomic_publication_restores_old_install() {
  local case_dir="$TMP_ROOT/atomic-interrupt"
  mkdir -p "$case_dir/server/worlds_local"
  printf '%s\n' "old install" > "$case_dir/server/worlds_local/world.db"
  mark_owned_reference_cache "$case_dir/server"

  run_setup atomic-interrupt valheim_publish_interrupt
  assert_nonzero "$RUN_STATUS" "an interrupted atomic publication must fail setup" || return 1
  assert_file "$RUN_CASE_DIR/state/publish-interrupt-attempted" "test must reach the atomic publication boundary" || return 1
  assert_file "$RUN_CASE_DIR/server/worlds_local/world.db" "signal cleanup must restore the old install" || return 1
  assert_equals "old install" "$(cat "$RUN_CASE_DIR/server/worlds_local/world.db")" "signal rollback must retain old install content" || return 1
  assert_no_server_temporary_state || return 1
}

test_signal_after_first_atomic_rename_restores_old_install() {
  local case_dir="$TMP_ROOT/first-rename-interrupt"
  mkdir -p "$case_dir/server/worlds_local"
  printf '%s\n' "old install" > "$case_dir/server/worlds_local/world.db"
  mark_owned_reference_cache "$case_dir/server"

  run_setup first-rename-interrupt valheim_first_rename_interrupt
  assert_nonzero "$RUN_STATUS" "a signal after the first publication rename must fail setup" || return 1
  assert_file "$RUN_CASE_DIR/state/first-rename-interrupt-attempted" "test must interrupt immediately after the old install is renamed" || return 1
  assert_file "$RUN_CASE_DIR/server/worlds_local/world.db" "first-rename signal cleanup must restore the old install" || return 1
  assert_equals "old install" "$(cat "$RUN_CASE_DIR/server/worlds_local/world.db")" "first-rename signal rollback must retain old install content" || return 1
  assert_no_server_temporary_state || return 1
}

test_steamcmd_download_is_completed_before_tar_reads_it() {
  run_setup steamcmd-download first_success "linux windows" false
  assert_equals 0 "$RUN_STATUS" "completed SteamCMD archive should install and run" || return 1
  assert_file "$RUN_CASE_DIR/state/tar-input" "tar should receive an archive file" || return 1
  assert_equals "COMPLETE_ARCHIVE" "$(cat "$RUN_CASE_DIR/state/tar-input")" "tar must see only the completed retry result" || return 1
  assert_no_steamcmd_temporary_state || return 1
}

test_failed_steamcmd_download_cleans_partial_archive() {
  run_setup steamcmd-download-failure steamcmd_download_failure "linux windows" false
  assert_nonzero "$RUN_STATUS" "failed SteamCMD download must fail setup before extraction" || return 1
  if [ -f "$RUN_CASE_DIR/state/tar-input" ]; then
    printf 'ASSERT: tar must not inspect a failed SteamCMD download\n' >&2
    return 1
  fi
  assert_no_steamcmd_temporary_state || return 1
}

test_failed_steamcmd_extraction_cleans_archive_and_stage() {
  run_setup steamcmd-extract-failure steamcmd_extract_failure "linux windows" false
  assert_nonzero "$RUN_STATUS" "failed SteamCMD extraction must fail setup" || return 1
  assert_file "$RUN_CASE_DIR/state/tar-input" "the extraction failure test must reach tar" || return 1
  assert_no_steamcmd_temporary_state || return 1
}

test_failed_steamcmd_publish_cleans_archive_and_extract_directory() {
  run_setup steamcmd-publish-failure steamcmd_publish_failure "linux windows" false
  assert_nonzero "$RUN_STATUS" "failed SteamCMD publish must fail setup" || return 1
  assert_no_steamcmd_temporary_state || return 1
}

test_partial_steamcmd_publication_is_removed_before_next_run() {
  run_setup steamcmd-partial-publish steamcmd_partial_publish_failure "linux windows" false
  assert_nonzero "$RUN_STATUS" "partial SteamCMD publication must fail setup" || return 1
  if [ -e "$RUN_CASE_DIR/steamcmd/steamcmd.sh" ]; then
    printf 'ASSERT: failed SteamCMD publication left an executable final destination\n' >&2
    return 1
  fi
  assert_no_steamcmd_temporary_state || return 1

  run_setup steamcmd-partial-publish first_success "linux windows" false
  assert_equals 0 "$RUN_STATUS" "the next run must reinstall instead of trusting a partial executable" || return 1
  assert_file "$RUN_CASE_DIR/steamcmd/.takaro-steamcmd-complete" "successful atomic publication should write its completion marker" || return 1
  if [ -f "$RUN_CASE_DIR/state/poisoned-steamcmd-ran" ]; then
    printf 'ASSERT: a poisoned partial SteamCMD executable was trusted on retry\n' >&2
    return 1
  fi
}

test_signal_during_partial_steamcmd_publication_cleans_destination() {
  run_setup steamcmd-partial-interrupt steamcmd_partial_publish_interrupt "linux windows" false
  assert_nonzero "$RUN_STATUS" "interrupted SteamCMD publication must fail setup" || return 1
  assert_file "$RUN_CASE_DIR/state/steamcmd-publish-interrupt-attempted" "test must reach the partial SteamCMD publication boundary" || return 1
  if [ -e "$RUN_CASE_DIR/steamcmd/steamcmd.sh" ]; then
    printf 'ASSERT: interrupted SteamCMD publication left an executable final destination\n' >&2
    return 1
  fi
  assert_no_steamcmd_temporary_state || return 1
}

test_markerless_steamcmd_executable_is_repaired_without_losing_unrelated_files() {
  local case_dir="$TMP_ROOT/steamcmd-markerless-repair"
  mkdir -p "$case_dir/steamcmd"
  write_poisoned_steamcmd_destination "$case_dir/steamcmd"
  printf '%s\n' 'keep me' > "$case_dir/steamcmd/unrelated.txt"

  run_setup steamcmd-markerless-repair first_success "linux windows" false
  assert_equals 0 "$RUN_STATUS" "a markerless executable must be repaired instead of trusted" || return 1
  assert_file "$RUN_CASE_DIR/steamcmd/.takaro-steamcmd-complete" "repaired SteamCMD install should have a completion marker" || return 1
  assert_file "$RUN_CASE_DIR/steamcmd/unrelated.txt" "repair should preserve unrelated caller files" || return 1
  assert_equals "keep me" "$(cat "$RUN_CASE_DIR/steamcmd/unrelated.txt")" "repair should preserve unrelated file contents" || return 1
  if [ -f "$RUN_CASE_DIR/state/poisoned-steamcmd-ran" ]; then
    printf 'ASSERT: markerless SteamCMD executable was invoked before repair\n' >&2
    return 1
  fi
}

test_complete_cached_steamcmd_install_is_reused() {
  run_setup steamcmd-complete-cache first_success "linux windows" false
  assert_equals 0 "$RUN_STATUS" "initial managed SteamCMD setup should succeed" || return 1
  assert_file "$RUN_CASE_DIR/steamcmd/.takaro-steamcmd-complete" "managed SteamCMD setup should publish a completion marker" || return 1
  assert_file "$RUN_CASE_DIR/server/$REFERENCE_CACHE_MARKER_NAME" "managed Valheim cache should publish an ownership marker" || return 1
  assert_equals 1 "$(cat "$RUN_CASE_DIR/state/steamcmd-download-count")" "initial setup should download SteamCMD once" || return 1

  rm -rf "$RUN_CASE_DIR/server/valheim_server_Data/Managed"
  rm -f "$RUN_CASE_DIR/state/count" "$RUN_CASE_DIR/state/platforms"
  run_setup steamcmd-complete-cache first_success "linux windows" false
  assert_equals 0 "$RUN_STATUS" "a complete cached SteamCMD install should remain usable" || return 1
  assert_equals 1 "$(cat "$RUN_CASE_DIR/state/steamcmd-download-count")" "complete cached install should not be downloaded again" || return 1
  assert_file "$RUN_CASE_DIR/server/$REFERENCE_CACHE_MARKER_NAME" "owned cache repair should retain its completion marker" || return 1
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
  test_owned_reference_cache_linux_windows_fallback_preserves_cache_data \
  test_invalid_legacy_live_server_is_refused_before_steamcmd_and_unchanged \
  test_missing_required_dlls_exhausts_all_attempts \
  test_empty_required_dlls_exhaust_all_attempts \
  test_corrupt_required_dlls_exhaust_all_attempts \
  test_fake_managed_markers_do_not_satisfy_real_assembly_validation \
  test_empty_bepinex_dlls_fail_setup \
  test_corrupt_bepinex_dlls_fail_setup \
  test_fake_bepinex_markers_do_not_satisfy_real_assembly_validation \
  test_failed_file_validator_is_actionable \
  test_missing_file_command_fails_preflight \
  test_valid_existing_install_skips_steamcmd \
  test_failed_atomic_publication_rolls_back_and_next_run_retries \
  test_failed_first_publication_does_not_forge_cache_ownership \
  test_signal_after_first_atomic_rename_restores_old_install \
  test_signal_during_atomic_publication_restores_old_install \
  test_steamcmd_download_is_completed_before_tar_reads_it \
  test_failed_steamcmd_download_cleans_partial_archive \
  test_failed_steamcmd_extraction_cleans_archive_and_stage \
  test_failed_steamcmd_publish_cleans_archive_and_extract_directory \
  test_partial_steamcmd_publication_is_removed_before_next_run \
  test_signal_during_partial_steamcmd_publication_cleans_destination \
  test_markerless_steamcmd_executable_is_repaired_without_losing_unrelated_files \
  test_complete_cached_steamcmd_install_is_reused \
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
