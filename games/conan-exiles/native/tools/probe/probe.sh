#!/usr/bin/env bash
# Host-side client for the probe REPL. The REPL listens on 127.0.0.1 inside the server
# container, so requests run through `docker exec` with the container's own node; the token is
# read inside the container from <ProbeDir>/token and never passes through the command line.
# Usage: probe.sh METHOD PATH [JSON-BODY]
#   probe.sh GET '/find?class=GameSession'
#   probe.sh POST /call '{"object":"first:ConanPlayerController","function":"K2_GetActorLocation"}'
set -euo pipefail
C=${PROBE_CONTAINER:-takaro-runner349-conan}
exec docker exec -i -e PORT="${PROBE_PORT:-7979}" -e TD="${PROBE_DIR_IN_CONTAINER:-/conan/ConanSandbox/Saved/TakaroProbe}" "$C" node -e '
const [m, p] = process.argv.slice(1);
const tok = require("fs").readFileSync(process.env.TD + "/token", "utf8").trim();
let body = "";
process.stdin.on("data", d => body += d).on("end", async () => {
  const r = await fetch("http://127.0.0.1:" + process.env.PORT + p, {
    method: m, headers: {"X-Probe-Token": tok, "Content-Type": "application/json"},
    body: (m === "GET" || m === "DELETE") ? undefined : body,
    signal: AbortSignal.timeout(600000)});
  process.stdout.write(await r.text());
});' "$1" "$2" <<< "${3:-}"
