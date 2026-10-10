#!/usr/bin/env bash
# Wine integration test for the native Takaro connector: the real dbghelp.dll, loaded by a tiny host exe under
# the pinned Enshrouded Proton image (GE-Proton10-30), talks WSS to a fake Takaro (tests/fake_takaro_ws.py) with a
# test CA. No game runs, so game capabilities degrade; transport, bridge, outbox, log-tail fallback and the
# diagnostics endpoint are the real code.
#
#   tests/wine/run.sh            build, run every check, tear down (exit 0 = all passed)
#   KEEP=1 tests/wine/run.sh     leave the containers for inspection
#   DLL=<path> tests/wine/run.sh test a prebuilt dbghelp.dll instead of building one
#
# Containers, network and image are named ensh-native-wine-*; nothing else is touched.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MOD=$(cd "$HERE/../.." && pwd)
IMG=${ENSHROUDED_IMAGE:-mornedhels/enshrouded-server:1.7.2-proton@sha256:85978a10f88a85ab0a0aa92e9821d30424895d38bf81fe543532451219c42d0d}
PYIMG=ensh-native-wine-py:local
NET=ensh-native-wine-net
RT=ensh-native-wine-rt
FAKE=ensh-native-wine-fake
ZIG=${ZIG:-$HOME/.local/opt/zig-linux-x86_64-0.13.0/zig}
WORK=${WORK:-$(mktemp -d /tmp/ensh-native-wine.XXXXXX)}
SRV=$WORK/srv
CERTS=$WORK/certs
RESULTS=$WORK/results
mkdir -p "$SRV/takaro" "$SRV/logs" "$CERTS" "$RESULTS"
chmod -R a+rwX "$SRV"
FAILED=()

log() { printf '[wine-test] %s\n' "$*"; }
check() {  # check <name> <condition command...>
  local name=$1; shift
  if "$@"; then log "PASS $name"; else log "FAIL $name"; FAILED+=("$name"); fi
}

# shellcheck disable=SC2317,SC2329  # invoked by the EXIT trap below
teardown() {
  [ -n "${KEEP:-}" ] && { log "KEEP=1: containers left running, work dir $WORK"; return; }
  # files the Wine process created belong to its uid: hand them back before the host cleans up
  docker exec "$RT" chmod -R a+rwX /opt/enshrouded/server/native-test >/dev/null 2>&1 || true
  docker rm -f "$RT" "$FAKE" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap teardown EXIT

# ---- build -------------------------------------------------------------------------------------------------
if [ -n "${DLL:-}" ]; then cp "$DLL" "$SRV/dbghelp.dll"; else "$MOD/build.sh" >/dev/null && cp "$MOD/build/dbghelp.dll" "$SRV/"; fi
"$ZIG" c++ -target x86_64-windows-gnu -O2 -std=c++17 -o "$SRV/host.exe" "$HERE/host.cpp"
rm -f "$SRV"/*.pdb "$SRV"/*.lib
log "dbghelp.dll sha256 $(sha256sum "$SRV/dbghelp.dll" | cut -c1-64)"

# ---- certificates: a test CA, a wrong CA, and a server cert for fake-takaro ---------------------------------
(
  cd "$CERTS"
  for ca in ca wrongca; do
    openssl req -x509 -newkey rsa:2048 -nodes -keyout $ca.key -out $ca.pem -days 2 -subj "/CN=Native Wine Test $ca" \
      -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
  done
  openssl req -newkey rsa:2048 -nodes -keyout fake-takaro.key -out fake-takaro.csr -subj "/CN=fake-takaro" 2>/dev/null
  printf "subjectAltName=DNS:fake-takaro\nextendedKeyUsage=serverAuth\n" > fake-takaro.ext
  openssl x509 -req -in fake-takaro.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out fake-takaro.pem -days 2 \
    -extfile fake-takaro.ext 2>/dev/null
  chmod 644 ./*
)

# ---- containers ----------------------------------------------------------------------------------------------
if ! docker image inspect "$PYIMG" >/dev/null 2>&1; then
  docker rm -f ensh-native-wine-pybase >/dev/null 2>&1 || true
  docker run -d --name ensh-native-wine-pybase python:3.12-slim sleep infinity >/dev/null
  docker exec ensh-native-wine-pybase pip install -q --root-user-action=ignore websockets==15.0.1
  docker commit ensh-native-wine-pybase "$PYIMG" >/dev/null && docker rm -f ensh-native-wine-pybase >/dev/null
fi
docker rm -f "$RT" "$FAKE" >/dev/null 2>&1 || true
docker network inspect "$NET" >/dev/null 2>&1 || docker network create "$NET" >/dev/null
docker run -d --name "$RT" --network "$NET" --entrypoint sleep -e WINEDLLOVERRIDES=dbghelp=n,b -e TZ=Etc/UTC \
  -v "$SRV":/opt/enshrouded/server/native-test -v "$CERTS":/certs:ro "$IMG" infinity >/dev/null
# What enshrouded-server does before `proton runinprefix`, with stdin from /dev/null (this Wine build segfaults in
# wine64-preloader when stdin is closed; supervisord gives the game /dev/null too).
docker exec -i "$RT" tee /usr/local/bin/run-native-host >/dev/null <<'EOF'
#!/bin/bash
set -e
export HOME=/home/enshrouded USER=enshrouded LANG=en_US.UTF-8
install_path=/opt/enshrouded/server; steam_app_id=2278520
export WINEDEBUG=${WINEDEBUG:--all}
export STEAM_COMPAT_CLIENT_INSTALL_PATH="/home/enshrouded/.steam/steam"
export STEAM_COMPAT_DATA_PATH="$install_path/steamapps/compatdata/$steam_app_id"
export WINEPREFIX="${STEAM_COMPAT_DATA_PATH}/pfx"
mkdir -p "$STEAM_COMPAT_DATA_PATH"
cd "$install_path/native-test"
exec proton runinprefix "$install_path/native-test/host.exe" "$@" </dev/null
EOF
docker exec "$RT" bash -c 'chmod +x /usr/local/bin/run-native-host && mkdir -p /opt/enshrouded/server/steamapps &&
  chown enshrouded:enshrouded /opt/enshrouded /opt/enshrouded/server && chown -R enshrouded:enshrouded /opt/enshrouded/server/steamapps /home/enshrouded'

start_fake() {  # start_fake <scenario>
  docker rm -f "$FAKE" >/dev/null 2>&1 || true
  docker run -d --name "$FAKE" --network "$NET" --network-alias fake-takaro --network-alias wrong-host \
    -e FAKE_SCENARIO="$1" -e FAKE_WS_PING_S=5 -v "$CERTS":/certs:ro -v "$MOD/tests":/t:ro -v "$SRV":/srv \
    "$PYIMG" python -u /t/fake_takaro_ws.py >/dev/null
  for _ in $(seq 1 50); do docker logs "$FAKE" 2>/dev/null | grep -q '"ev": "listen"' && return; sleep 0.2; done
  log "fake Takaro did not start"; docker logs "$FAKE"; exit 1
}

run_host() {  # run_host <seconds> <log> [-e VAR=value ...]
  local seconds=$1 out=$2; shift 2
  docker exec -u enshrouded -e TAKARO_WS_URL=wss://fake-takaro:8443/ -e TAKARO_IDENTITY_TOKEN=wine-identity \
    -e TAKARO_REGISTRATION_TOKEN=wine-registration -e "TAKARO_SERVER_NAME=Wine Test" -e TAKARO_PLUGIN_TOKEN=wine-plugin-token \
    -e TAKARO_RECONNECT_BASE_MS=1000 -e 'TAKARO_CA_FILE=Z:\certs\ca.pem' "$@" "$RT" timeout $((seconds + 60)) run-native-host "$seconds" >"$out" 2>&1 || true
}

reset_state() {
  docker exec "$RT" bash -c 'rm -rf /opt/enshrouded/server/native-test/takaro/* && printf "[I 00:00:00,001] boot\n" > /opt/enshrouded/server/native-test/logs/enshrouded_server.log && chmod -R a+rwX /opt/enshrouded/server/native-test'
}

health() {  # health <path> [token]
  docker exec "$RT" curl -s -m 5 ${2:+-H "Authorization: Bearer $2"} -w '\n%{http_code}' "http://127.0.0.1:18890$1" || true
}

# ---- 1. the main scenario ------------------------------------------------------------------------------------
log "main scenario: identify, ping/pong, requests, event confirm-on-later-pong, abort+resend, clean close+reconnect, dead link"
reset_state
start_fake native
run_host 180 "$RESULTS/host-main.txt" &
HOST=$!
# diagnostics while it runs: token-gated /health with native + perf, action routes off by default
for _ in $(seq 1 60); do
  health /health wine-plugin-token >"$RESULTS/health.txt"
  grep -q '"identified":true' "$RESULTS/health.txt" && break
  sleep 1
done
health /health >"$RESULTS/health-noauth.txt"
health /players wine-plugin-token >"$RESULTS/players-http.txt"
health /debug/perf wine-plugin-token >"$RESULTS/perf.txt"
docker wait "$FAKE" >"$RESULTS/fake-exit.txt"
docker logs "$FAKE" >"$RESULTS/fake-main.txt" 2>&1
docker exec "$RT" pkill -f host.exe >/dev/null 2>&1 || true
wait $HOST || true
cp "$SRV/takaro/plugin.log" "$RESULTS/plugin-main.log" 2>/dev/null || true
grep '"ev": "check"' "$RESULTS/fake-main.txt" | sed 's/^/    /' || true
check "fake-takaro-scenario" grep -q '"ev": "result", "ok": true' "$RESULTS/fake-main.txt"
check "health-native-identified" grep -q '"identified":true' "$RESULTS/health.txt"
check "health-perf-present" grep -q '"perf":{"windowS"' "$RESULTS/health.txt"
check "health-no-secrets" bash -c "! grep -q -e wine-identity -e wine-registration '$RESULTS/health.txt'"
check "health-needs-token" bash -c "tail -1 '$RESULTS/health-noauth.txt' | grep -q 401"
check "action-routes-off-over-http" bash -c "tail -1 '$RESULTS/players-http.txt' | grep -q 404"
check "debug-perf-served" bash -c "tail -1 '$RESULTS/perf.txt' | grep -q 200"
check "plugin-log-async-transport" grep -q 'Takaro WebSocket upgraded (epoch 1, tls pinned-ca' "$RESULTS/plugin-main.log"
check "plugin-log-dead-link" grep -q 'ended: no message from Takaro for 20s (dead link)' "$RESULTS/plugin-main.log"
check "plugin-log-no-teardown-leak" bash -c "! grep -q 'teardown incomplete' '$RESULTS/plugin-main.log'"

# ---- 2. certificate rejection: wrong CA, then a missing CA file (fail closed, no system-trust fallback) ------
log "negative: wrong CA and missing CA file must never reach identify"
reset_state
start_fake count
run_host 12 "$RESULTS/host-wrongca.txt" -e 'TAKARO_CA_FILE=Z:\certs\wrongca.pem'
cp "$SRV/takaro/plugin.log" "$RESULTS/plugin-wrongca.log" 2>/dev/null || true
reset_state
run_host 8 "$RESULTS/host-missingca.txt" -e 'TAKARO_CA_FILE=Z:\certs\missing.pem'
cp "$SRV/takaro/plugin.log" "$RESULTS/plugin-missingca.log" 2>/dev/null || true
reset_state
run_host 12 "$RESULTS/host-wronghost.txt" -e TAKARO_WS_URL=wss://wrong-host:8443/
cp "$SRV/takaro/plugin.log" "$RESULTS/plugin-wronghost.log" 2>/dev/null || true
docker logs "$FAKE" >"$RESULTS/fake-count.txt" 2>&1
check "wrong-ca-rejected" grep -q 'TLS rejected' "$RESULTS/plugin-wrongca.log"
check "missing-ca-fails-closed" grep -q 'refusing to connect (no fallback to system trust)' "$RESULTS/plugin-missingca.log"
check "wrong-hostname-rejected" grep -q 'connect failed at send-request (WinHTTP 12157 TLS/certificate rejected)' "$RESULTS/plugin-wronghost.log"
check "no-identify-reached-fake" bash -c "! grep -q '\"ev\": \"identify\"' '$RESULTS/fake-count.txt'"

# ---- 3. not configured: the plugin runs, the connection stays off --------------------------------------------
log "unconfigured: no tokens -> connection off, plugin still serves /health"
reset_state
run_host 8 "$RESULTS/host-unconfigured.txt" -e TAKARO_IDENTITY_TOKEN= -e TAKARO_REGISTRATION_TOKEN= &
HOST=$!
sleep 6
health /health wine-plugin-token >"$RESULTS/health-unconfigured.txt"
wait $HOST || true
check "unconfigured-health-says-why" grep -q '"native":{"enabled":false,"reason":"not connected: no registration token' "$RESULTS/health-unconfigured.txt"

# ---- 4. plugin.json while running: fresh install, wrong then right token, upgrade that replaced the file -----
log "live config: shipped plugin.json, token pasted while running, upgrade keeps the identity"
reset_state
docker exec -u enshrouded -i "$RT" sh -c 'cat > /opt/enshrouded/server/native-test/takaro/plugin.json' <"$MOD/../scripts/templates/plugin.json"
printf '{"name": "Wine World"}\n' | docker exec -u enshrouded -i "$RT" sh -c 'cat > /opt/enshrouded/server/native-test/enshrouded_server.json'
start_fake accept
LIVE_ENV=(-e TAKARO_IDENTITY_TOKEN= -e TAKARO_REGISTRATION_TOKEN= -e TAKARO_SERVER_NAME= -e TAKARO_CONFIG_POLL_MS=1000)
PJ=/opt/enshrouded/server/native-test/takaro/plugin.json
set_token() { docker exec -u enshrouded "$RT" sed -i "s/\"registrationToken\": \"[^\"]*\"/\"registrationToken\": \"$1\"/" "$PJ"; }
# shellcheck disable=SC2329  # invoked through check
wait_for() {  # wait_for <seconds> <grep args...>
  local s=$1; shift
  for _ in $(seq 1 "$s"); do grep -q "$@" && return 0; sleep 1; done; return 1
}
run_host 45 "$RESULTS/host-live.txt" "${LIVE_ENV[@]}" &
HOST=$!
check "live-no-token-banner" wait_for 20 'registrationToken not set, the server is not connected to Takaro' "$RESULTS/host-live.txt"
check "live-banner-names-file" grep -q 'into /opt/enshrouded/server/native-test/takaro/plugin.json' "$RESULTS/host-live.txt"
GEN_ID=$(docker exec "$RT" sed -n 's/.*"identityToken": "\([^"]*\)".*/\1/p' "$PJ")
check "live-identity-generated" bash -c "echo '$GEN_ID' | grep -Eq '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'"
check "live-no-identify-without-token" bash -c "! docker logs '$FAKE' 2>&1 | grep -q '\"ev\": \"identify\"'"
set_token wine-wrong-token
check "live-wrong-token-refused" wait_for 20 'Takaro refused this server' "$RESULTS/host-live.txt"
set_token wine-registration
check "live-fixed-token-connects" wait_for 20 "connected to Takaro as \"Wine World (${GEN_ID:0:8})\"" "$RESULTS/host-live.txt"
check "live-identify-carried-generated-identity" bash -c "docker logs '$FAKE' 2>&1 | grep '\"ok\": true' | grep -q '$GEN_ID'"
sleep 3
check "live-saved-copy-has-token" docker exec "$RT" grep -q '"registrationToken": "wine-registration"' /opt/enshrouded/server/native-test/takaro/connector-state/saved-settings.json
check "live-plugin-json-no-copied-token" bash -c "docker exec '$RT' grep -c wine-registration '$PJ' | grep -q '^1$'"
docker exec "$RT" pkill -f host.exe >/dev/null 2>&1 || true
wait $HOST || true
cp "$SRV/takaro/plugin.log" "$RESULTS/plugin-live.log" 2>/dev/null || true
check "live-log-no-tokens" bash -c "! grep -q -e wine-registration -e wine-wrong-token '$RESULTS/plugin-live.log' '$RESULTS/host-live.txt'"
# The upgrade copied the whole zip, shipped plugin.json included: same identity, token from the saved copy.
docker exec -u enshrouded -i "$RT" sh -c "cat > $PJ" <"$MOD/../scripts/templates/plugin.json"
docker logs "$FAKE" >"$RESULTS/fake-live-1.txt" 2>&1
run_host 15 "$RESULTS/host-live-upgrade.txt" "${LIVE_ENV[@]}"
docker logs "$FAKE" >"$RESULTS/fake-live.txt" 2>&1
check "live-upgrade-reconnects" grep -q 'connected to Takaro as' "$RESULTS/host-live-upgrade.txt"
check "live-upgrade-same-identity" bash -c "[ \$(grep '\"ok\": true' '$RESULTS/fake-live.txt' | grep -c '$GEN_ID') -gt \$(grep '\"ok\": true' '$RESULTS/fake-live-1.txt' | grep -c '$GEN_ID') ]"
check "live-upgrade-identity-written-back" docker exec "$RT" grep -q "\"identityToken\": \"$GEN_ID\"" "$PJ"

log "results in $RESULTS"
if [ ${#FAILED[@]} -eq 0 ]; then log "ALL PASSED"; exit 0; fi
log "FAILED: ${FAILED[*]}"
exit 1
