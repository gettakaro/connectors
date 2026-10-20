#!/usr/bin/env bash
# Boots a local TF2 dedicated server with the connector against the fake Takaro in
# tests/live-harness.mjs and fails unless every scripted check passes.
#
#   TF2_SERVER_ROOT=/path/to/tf2-server TF2_PACKAGE=<release zip> games/tf2/scripts/run-live-smoke.sh
#
# TF2_SERVER_ROOT is a TF2 dedicated server (the folder with srcds_run) with MetaMod:Source 1.12
# and SourceMod 1.13 installed. TF2_PACKAGE is a Linux release zip from build-release.sh. The
# harness needs the ws npm package: npm install --prefix games/tf2/_data/harness ws@8
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
server_root="${TF2_SERVER_ROOT:?set TF2_SERVER_ROOT to a TF2 dedicated server with MetaMod and SourceMod}"
package="${TF2_PACKAGE:?set TF2_PACKAGE to a takaro-tf2-linux-*.zip built by build-release.sh}"
harness_port="${TF2_HARNESS_PORT:-23826}"
server_port="${TF2_SERVER_PORT:-27016}"
sv_lan="${TF2_SV_LAN:-0}"
timeout_seconds="${TF2_SMOKE_TIMEOUT_SECONDS:-120}"
logs="$project_root/_data/logs"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
harness_log="$logs/tf2-live-harness-$timestamp.jsonl"
server_log="$logs/tf2-live-server-$timestamp.log"
server_stdin="$logs/tf2-live-server-stdin-$timestamp.fifo"
cfg="$server_root/tf/cfg/sourcemod/takaro_tf2.cfg"
cfg_backup="$logs/takaro_tf2.cfg.before-$timestamp"

require_file() {
  [ -f "$1" ] || {
    echo "Missing required file: $1" >&2
    exit 1
  }
}

require_file "$server_root/srcds_run"
require_file "$server_root/tf/addons/sourcemod/bin/sourcemod_mm_i486.so"
require_file "$package"
require_file "${TF2_HARNESS_WS:-$project_root/_data/harness/node_modules/ws}/package.json"

mkdir -p "$logs"
[ -f "$cfg" ] && cp "$cfg" "$cfg_backup"
python3 -I -m zipfile -e "$package" "$server_root/tf"

mkfifo "$server_stdin"
exec 3<>"$server_stdin"

cat > "$cfg" <<CFG
takaro_tf2_ws_url "ws://127.0.0.1:$harness_port"
takaro_tf2_server_name "TF2 Local Smoke Server"
takaro_tf2_identity_token ""
takaro_tf2_registration_token ""
takaro_tf2_include_bots "1"
takaro_tf2_reconnect_min_ms "1000"
takaro_tf2_reconnect_max_ms "30000"
takaro_tf2_command_allowlist_exact "status;tf_bot_add 1 scout red normal TakaroLiveBot noquota;tf_bot_kill all"
takaro_tf2_command_allowlist_prefixes ""
takaro_tf2_shutdown_enabled "0"
CFG

node "$project_root/tests/live-harness.mjs" \
  --port "$harness_port" \
  --log "$harness_log" \
  --timeout-ms "$((timeout_seconds * 1000))" &
harness_pid=$!

cleanup() {
  trap - EXIT
  if [ -n "${server_pid:-}" ] && kill -0 "$server_pid" >/dev/null 2>&1; then
    printf 'quit\n' >&3 || true
    for _ in 1 2 3 4 5; do
      kill -0 "$server_pid" >/dev/null 2>&1 || break
      sleep 1
    done
  fi
  if [ -n "${server_pid:-}" ] && kill -0 "$server_pid" >/dev/null 2>&1; then
    pkill -TERM -P "$server_pid" >/dev/null 2>&1 || true
    kill -TERM "$server_pid" >/dev/null 2>&1 || true
    sleep 1
  fi
  if [ -n "${server_pid:-}" ] && kill -0 "$server_pid" >/dev/null 2>&1; then
    pkill -KILL -P "$server_pid" >/dev/null 2>&1 || true
    kill -KILL "$server_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${server_pid:-}" ]; then
    wait "$server_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${harness_pid:-}" ] && kill -0 "$harness_pid" >/dev/null 2>&1; then
    kill "$harness_pid" >/dev/null 2>&1 || true
    wait "$harness_pid" >/dev/null 2>&1 || true
  fi
  exec 3>&- || true
  rm -f "$server_stdin"
  # Put the operator's config back (or the shipped template when there was none).
  if [ -f "$cfg_backup" ]; then
    mv -f "$cfg_backup" "$cfg"
  else
    cp "$project_root/mod/cfg/sourcemod/takaro_tf2.cfg" "$cfg"
  fi
}
trap cleanup EXIT

(
  cd "$server_root"
  timeout --foreground "${timeout_seconds}s" ./srcds_run \
    -game tf \
    -console \
    -usercon \
    -insecure \
    -port "$server_port" \
    +sv_lan "$sv_lan" \
    +maxplayers 4 \
    +tf_bot_join_after_player 0 \
    +map ctf_2fort < "$server_stdin"
) > >(tee "$server_log") 2>&1 &
server_pid=$!

if wait "$harness_pid"; then
  cleanup
  echo "TF2 live smoke passed"
  echo "Harness log: $harness_log"
  echo "Server log: $server_log"
else
  echo "TF2 live smoke failed"
  echo "Harness log: $harness_log"
  echo "Server log: $server_log"
  exit 1
fi
