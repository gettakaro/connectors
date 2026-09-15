# Takaro Conan Mod Install, Reconnect, and Live Test Runbook

This runbook assumes a cooked `TakaroConan.pak` already exists.

If a valid registration token or cooked build-host artifact is still missing,
start with `CURRENT_BLOCKERS_AND_REQUIRED_INPUTS.md` before changing modlists or
restarting the server.

## Paths

Server mods:

```text
/home/hendrik/gamingconnectors/.runtime/conan-server/ConanSandbox/Mods
```

Client mods:

```text
/home/hendrik/.local/share/Steam/steamapps/common/Conan Exiles/ConanSandbox/Mods
```

Server config:

```text
/home/hendrik/gamingconnectors/.runtime/conan-server/ConanSandbox/Saved/Config/LinuxServer/ServerSettings.ini
```

Server logs:

```text
/home/hendrik/gamingconnectors/.runtime/conan-server/ConanSandbox/Saved/Logs/ConanSandbox.log
/home/hendrik/gamingconnectors/.runtime/conan-server/ConanSandbox/Saved/Logs/RconCommandLog.log
```

Client log:

```text
/home/hendrik/.local/share/Steam/steamapps/common/Conan Exiles/ConanSandbox/Saved/Logs/ConanSandbox.log
```

Join address:

```text
192.168.129.13:7777
```

## Preflight

```bash
curl -fsS http://127.0.0.1:3010/health
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/check-sidecar-auth.sh
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/check-takaro-install-readiness.sh
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/check-takaro-mod-install.sh
```

For a repeatable checkpoint loop while applying a new token, importing a
returned artifact, restarting the server, and reconnecting the client, run:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/loop-takaro-mod-goal.sh
```

For one non-destructive checkpoint pass:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/loop-takaro-mod-goal.sh --once
```

The loop writes timestamped logs under `.runtime/conan-goal-loop/`, uses
isolated temporary sidecars for token-candidate probes, and does not apply
tokens, stop processes, edit modlists, restart the server, or print token
values. It exits successfully only when the strict final audit passes.

Before replacement, both install readiness and install validation are expected
to fail because Pippi is still installed/running and no `TakaroConan.pak` is
installed. Readiness should pass before applying the returned artifact, except
for the deliberate final replacement steps that happen during the install
window.

If sidecar auth fails because the registration token is stale, validate the new
active-domain token before writing runtime config:

```bash
TAKARO_CONAN_NEW_REGISTRATION_TOKEN_FILE=/path/to/token \
  node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-registration-token.mjs
TAKARO_CONAN_NEW_REGISTRATION_TOKEN_FILE=/path/to/token \
  node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-registration-token.mjs --apply --restart --kill-existing
```

The first command is a no-write validation using a temporary sidecar on an
isolated health port. The second command writes the runtime config and replaces
the running sidecar only after temporary validation succeeds, then waits for the
real `/health` endpoint to prove the restarted sidecar is identified by Takaro
and exposes the final TakaroConan bridge guards:
`modBridge.sourceAttributionRequired=true` and
`modBridge.gameEventValidationEnabled=true`.

Once both a fresh token and returned build-host artifact are available, the
guarded combined workflow can validate both without writes:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh \
  --token-file /path/to/token \
  --artifact-from /path/to/build-host-return
```

After that dry run passes, the same wrapper can apply the token, install the
artifact, and stop the old host Pippi/chat poller. It must not run the strict
checkpoint in the same invocation because Conan only loads the new modlist after
the dedicated server restarts and the client fully restarts/rejoins:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh \
  --token-file /path/to/token --apply-token \
  --artifact-from /path/to/build-host-return --apply-install \
  --stop-pippi-poller --skip-loop
```

After the dedicated server restart and client relog/rejoin, run the post-reconnect
strict checkpoint:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh \
  --post-reconnect-check
```

## Install TakaroConan.pak

If the build host returned a folder or archive containing `TakaroConan.pak`,
`artifact-manifest.json`, `BUILD_REPORT.md`, and `SOURCE_EVIDENCE.md`, import
and stage it first:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/import-takaro-artifact.sh --from /path/to/build-host-return
```

The importer validates the returned manifest, SHA-256, build report, source
evidence, and pak intake before printing the exact installer command for the
staged artifact.

Set the staged artifact path printed by the importer:

```bash
PAK=/path/from/import/payload/TakaroConan.pak
SERVER_MODS="/home/hendrik/gamingconnectors/.runtime/conan-server/ConanSandbox/Mods"
CLIENT_MODS="/home/hendrik/.local/share/Steam/steamapps/common/Conan Exiles/ConanSandbox/Mods"
```

Do not manually copy the pak or edit modlists for final validation. Use
`apply-ready-takaro-inputs.sh` for the final replacement window so token
validation, artifact import, install dry-run, token apply/restart, pak install,
Pippi poller stop, restart/rejoin, and post-reconnect checkpoint stay in order.
The wrapper fails before token apply or client modlist mutation if the Conan
client is still running.

The installer copies the same artifact to server and client, preserves
`TakaroConan.artifact-manifest.json`, `TakaroConan.BUILD_REPORT.md`, and
`TakaroConan.SOURCE_EVIDENCE.md` beside the installed pak, backs up the previous
modlists/artifacts, and rewrites both modlists.

The low-level installer can still be used for a read-only dry run on a staged
artifact:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/install-takaro-mod.sh --pak "$PAK"
```

Do not use the low-level installer to bypass the guarded final apply flow. If
`install-takaro-mod.sh --apply` is called directly, it also fails before copying
files or rewriting modlists while the Conan client is still running.

Keep `Pippi.pak` on disk if useful for rollback, but it must not be listed in
either `modlist.txt` during Takaro mod validation. The old host Pippi poller
must be stopped by the guarded apply wrapper, not left for a later manual step.

Guarded apply path:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh \
  --token-file /path/to/token --apply-token \
  --artifact-from /path/to/build-host-return --apply-install \
  --stop-pippi-poller --skip-loop
```

The explicit validation command is read-only. The installer also runs it by
default, auto-detects `artifact-manifest.json`, `BUILD_REPORT.md`, and
`SOURCE_EVIDENCE.md` next to the staged pak, cross-checks the report and source
evidence against the pak/manifest, and writes an intake manifest next to the
modlist backups. The first installer command is a dry run. The `--apply` run
copies the pak, preserves the build manifest as
`TakaroConan.artifact-manifest.json`, preserves the build report as
`TakaroConan.BUILD_REPORT.md`, preserves the source evidence as
`TakaroConan.SOURCE_EVIDENCE.md`, backs up current modlists/artifacts, and
rewrites both modlists to `*TakaroConan.pak` when the wrapper invokes it in
apply mode.
`check-takaro-mod-install.sh` then verifies that the server and client paks
match each other, that their installed manifests and reports match each other,
that their installed source evidence files match each other, and that each
installed pak/report/source-evidence set matches the manifest SHA-256/size plus
DevKit compile/cook assertions.

## Restart and Reconnect

1. Fully exit the Conan client.
2. Stop the dedicated server.
3. Start the dedicated server from `/home/hendrik/gamingconnectors/.runtime/conan-server`:

```bash
./ConanSandboxServer.sh -log -server -nosteamclient -MULTIHOME=0.0.0.0 -Port=7777 -QueryPort=27015
```

4. Launch the Conan client through Steam.
5. Connect to `192.168.129.13:7777`.
6. Confirm MCP `gameserverGetPlayers` returns the reconnected player before targeted chat validation.

Conan loads mods at startup. Restart both sides after any modlist change.

## Required Proof

### Load Proof

Server log must show:

- `TakaroConan.pak` mounted;
- Takaro mod controller/component initialized;
- no Pippi mod controller loaded.

Client log must show:

- `TakaroConan.pak` mounted from the client `Mods` folder;
- no Pippi mod controller loaded.

Run the post-reconnect wrapper checkpoint:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh \
  --post-reconnect-check
```

The wrapper runs the install and live validators in the strict goal loop.
`validate-takaro-mod-live.sh` writes a JSON receipt to
`.runtime/conan-live-receipts/<timestamp>.json` by default. Set
`TAKARO_CONAN_LIVE_RECEIPT=/path/to/receipt.json` to choose the output path.
The receipt is intended for the QA ledger and contains current markers,
sanitized health/modBridge summaries, exact TakaroConan result/event proof
entries for the current server-wide, targeted, and inbound markers, game server
ID, player identity, fresh module command event proof, log offsets, and
pass/fail messages. It must not contain Takaro registration tokens, RCON
passwords, bearer tokens, or MCP session headers.

Validate the final successful receipt before claiming completion:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-live-receipt.sh --receipt /path/to/live-receipt.json
```

For current-state/debug receipts that are expected to fail, use:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-live-receipt.sh --receipt /path/to/live-receipt.json --allow-failed
```

Expected after replacement:

- server/client paks exist;
- server/client modlists contain exactly `*TakaroConan.pak`;
- no `Pippi.pak` entry in either modlist;
- health is reachable;
- Pippi host poller is not running.

### Bridge Proof

```bash
curl -fsS http://127.0.0.1:3010/health
```

Expected:

- `ok=true`
- `takaroIdentified=true`
- `modBridge.connected=true`
- `modBridge.lastPollSource` contains `TakaroConan`
- `modBridge.lastResultSource` contains `TakaroConan` after a handled command
- `modBridge.lastResultAt` is after the current validation run started
- `modBridge.recentResults` contains both current outbound markers with
  `action="sendMessage"`, `source` containing `TakaroConan`, and
  `resultSuccess=true`
- `modBridge.lastEventSource` contains `TakaroConan` after inbound chat
- `modBridge.lastEventAt` is after the current validation run started
- `modBridge.recentEvents` contains the current inbound marker with
  `type="chat-message"`, `source` containing `TakaroConan`, and a stable
  Steam/platform identity
- `pendingCommands=0`
- `pendingResults=0`

`modBridge.connected=true` must be caused by the Takaro Conan mod polling `/mod/poll`, not by the old host Pippi poller. The mod should call `/mod/poll?source=TakaroConan` or send `X-Takaro-Mod-Source: TakaroConan`.

### Takaro MCP Proof

Use a fresh MCP session at `http://127.0.0.1:3000/mcp`.

Required non-destructive checks:

- `gameserverTestReachabilityForId`
- `gameserverGetPlayers`
- `gameserverListBans`
- `gameserverGetMapInfo`
- `gameserverGetMapTile`
- `gameserverExecuteCommand` with `help`
- `gameserverSendMessage` server-wide
- `gameserverSendMessage` targeted to the online player

Mutation checks are approval-gated:

- `gameserverGiveItem`
- `gameserverTeleportPlayer`
- `gameserverKickPlayer`
- `gameserverBanPlayer`
- `gameserverUnbanPlayer`
- `gameserverShutdown`

### Player-Visible Proof

Server-wide chat:

- send a unique Takaro message;
- player sees it in normal chat feed;
- logs show the Takaro mod handled the command.

Targeted chat:

- send a unique targeted message to the online player;
- target sees it;
- no Pippi `directmessage` command appears in RCON logs.

Inbound chat:

- player sends a unique message;
- connector emits `chat-message`;
- Takaro accepts it without validation errors;
- event includes stable Steam/platform identity.
- `/health` shows `modBridge.lastEventSource` containing `TakaroConan` and `modBridge.lastEventType="chat-message"`.
- `/health.modBridge.recentEvents` contains the same current inbound marker
  with stable Steam/platform identity.

The strict validator prints a fresh marker for each run. For repeatable operator
coordination, set one explicit marker and use the same exact strings for the
player-visible and inbound confirmations after the client proof exists:

```bash
MARKER="TAKARO_CONAN_MOD_LIVE_$(date -u +%Y%m%dT%H%M%SZ)"
TAKARO_CONAN_MESSAGE_MARKER="$MARKER" \
TAKARO_CONAN_SERVER_WIDE_VISIBLE_MARKER="$MARKER server-wide" \
TAKARO_CONAN_TARGETED_VISIBLE_MARKER="$MARKER targeted" \
TAKARO_CONAN_INBOUND_CHAT_MARKER="$MARKER inbound" \
TAKARO_CONAN_MODULE_COMMAND_SENT="$MARKER module-command" \
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-takaro-mod-live.sh
```

The connected player must send exactly `$MARKER inbound` from Conan chat during
that same run. The connected player must also send one configured Takaro module
command from Conan chat during that same run before setting
`TAKARO_CONAN_MODULE_COMMAND_SENT="$MARKER module-command"`. Stale boolean
confirmations are not accepted.

### Module Proof

Installed modules to re-check:

- `teleports`
- `Waypoints`
- `serverMessages`

Before claiming module automation is done:

- installed module is visible for this game server;
- command item executes through Takaro and reaches connector/RCON or mod boundary;
- cronjob execution history succeeds;
- server-message output is visible in player chat or server logs;
- hook automation is either installed and proven or explicitly out of scope.

Use the command-event watcher during the client loop:

```bash
node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/list-module-command-candidates.mjs
node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/wait-module-command-evidence.mjs
```

Then send one of the listed `teleports` or `Waypoints` triggers from the Conan
client chat before the timeout expires. The watcher resolves the game server
through MCP, waits for a fresh `command-executed` or
`command-execution-denied` event, and does not print token values.

The final `validate-takaro-mod-live.sh` receipt now also requires this current
module command proof. A passed receipt must include
`proof.moduleCommandEventSeen=true` and at least one
`proof.recentModuleCommandEvents[]` entry with `eventName="command-executed"` or
`eventName="command-execution-denied"` whose `createdAt` is after the live
validation started.

## Rollback

To restore current Pippi-backed behavior:

```bash
SERVER_MODS="/home/hendrik/gamingconnectors/.runtime/conan-server/ConanSandbox/Mods"
CLIENT_MODS="/home/hendrik/.local/share/Steam/steamapps/common/Conan Exiles/ConanSandbox/Mods"
printf '*Pippi.pak\n' > "$SERVER_MODS/modlist.txt"
printf '*Pippi.pak\n' > "$CLIENT_MODS/modlist.txt"
```

Then restart server and client and restart the host Pippi poller.
