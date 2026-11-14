# Blueprint SDK research probe

Research only, not a product feature. Findings: [docs/research/blueprint-sdk/conan-exiles.md](../../../../docs/research/blueprint-sdk/conan-exiles.md).

The probe is part of the native connector (`native/core/conan/blueprint_probe.{h,cpp}`). It does
nothing unless the server process has `TAKARO_CONAN_BLUEPRINT_PROBE` set to a directory.
`TAKARO_CONAN_BLUEPRINT_PROBE_ORACLE=0` leaves out the counting hook subscriptions.

- `stub.py` stands in for Takaro. It answers `takaro run` requests through the Takaro REST command
  trigger and records `takaro event` requests. The API key comes from standard input only:

  ```bash
  echo "$KEY" | python3 stub.py <probe dir> <api base url> <domain id> <game server id>
  ```

- `drive.json` in the probe directory makes the probe call `ExecuteConsoleCommand` the way a
  Blueprint node would:

  ```json
  {"name": "d1", "mode": "pc" | "pc-admin" | "none", "command": "takaro run <steam64> @ping"}
  ```

  The result lands in `drive-result-<name>.json`.
- `takaro-module/sdkBridgeHook.js` is the research Takaro hook (`eventType: "log"`). It turns an
  engine console line into a command run as the player. Use it only with the anchored regex given
  in the findings: an unanchored match can be triggered from public chat.

Counters are under `blueprintProbe` in `events-health.json`. Hook hits are under `hooks`
(owner `blueprint-probe`).
