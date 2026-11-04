#!/bin/bash
# Starts EmpyrionDedicated.exe headless under Wine and streams its newest log to stdout.
set -euo pipefail
cd /empyrion
[ -f /wine/system.reg ] || { wineboot --init; wineserver -w; }
CFG="${EMPYRION_DEDICATED_YAML:-dedicated-takaro.yaml}"
mkdir -p Logs
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
Xvfb :99 -screen 0 640x480x16 >/dev/null 2>&1 &
export DISPLAY=:99
for _ in $(seq 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.1; done
# The dedi resolves ../Logs and ../PlayfieldServer from its working directory, so it must run from DedicatedServer/.
(cd DedicatedServer && exec wine EmpyrionDedicated.exe -batchmode -nographics -dedicated "../$CFG" -logFile ../Logs/dedi-stdout.log) &
DEDI=$!
graceful_stop() {
  # The dedi ignores signals under Wine; ask it to save and exit through its own telnet console.
  local pw port
  pw=$(sed -nE 's/^[[:space:]]*Tel_Pwd:[[:space:]]*([^#[:space:]]+).*/\1/p' "/empyrion/$CFG" | head -1)
  port=$(sed -nE 's/^[[:space:]]*Tel_Port:[[:space:]]*([0-9]+).*/\1/p' "/empyrion/$CFG" | head -1)
  if [ -n "$pw" ] && exec 3<>/dev/tcp/127.0.0.1/"${port:-30004}"; then
    sleep 1; printf '%s\r\n' "$pw" >&3; sleep 1; printf 'saveandexit 0\r\n' >&3; sleep 1; exec 3>&-
    # $DEDI is Wine's launcher, which outlives the game; watch the game processes themselves.
    # Under Wine the dedi usually crashes in Unity teardown after it has saved, and its crash handler keeps the
    # process alive. Once the scheduled quit has started and the log has been quiet for 5 s, the save is done.
    local quiet=0 last size
    last=$(stat -c %s Logs/dedi-stdout.log 2>/dev/null || echo 0)
    for _ in $(seq 90); do
      pgrep -f '^Z:.*(EmpyrionDedicated|EmpyrionPlayfieldServer)\.exe' >/dev/null || break
      sleep 1
      size=$(stat -c %s Logs/dedi-stdout.log 2>/dev/null || echo 0)
      if [ "$size" = "$last" ]; then quiet=$((quiet + 1)); else quiet=0; last=$size; fi
      if [ "$quiet" -ge 5 ] && tail -c 65536 Logs/dedi-stdout.log | grep -q 'Doing scheduled quit'; then break; fi
    done
  fi
  wineserver -k 2>/dev/null || true
}
trap 'graceful_stop' TERM INT
# A quit from inside the game (saveandexit, e.g. Takaro's shutdown action) ends in the same Wine teardown crash and
# would leave the container up with a hung dedi. End Wine once the quit has run and the log has gone quiet, so the
# container exits and its restart policy brings the server back.
quit_watch() {
  local quiet=0 last size
  last=$(stat -c %s Logs/dedi-stdout.log 2>/dev/null || echo 0)
  while sleep 1; do
    size=$(stat -c %s Logs/dedi-stdout.log 2>/dev/null || echo 0)
    if [ "$size" = "$last" ]; then quiet=$((quiet + 1)); else quiet=0; last=$size; fi
    if [ "$quiet" -ge 5 ] && tail -c 65536 Logs/dedi-stdout.log | grep -q 'Doing scheduled quit'; then
      wineserver -k 2>/dev/null || true
      return
    fi
  done
}
sleep 5
tail -F -n +1 Logs/dedi-stdout.log 2>/dev/null &
quit_watch &
wait $DEDI
