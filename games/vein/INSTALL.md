# Install and operate the native VEIN connector

The Linux dedicated server loads `libtakaro-vein.so` into `VeinServer-Linux-Test` with `LD_PRELOAD`. The library connects directly to Takaro. Players use the unmodified VEIN client. Windows dedicated servers and Workshop installation are not supported.

## Clean installation

1. Create a Takaro **Generic** game server and obtain its registration token. Stop the VEIN dedicated server and verify the client and server run the same VEIN game version. Steam client and server build IDs may differ.
2. Verify the release archive against the accompanying top-level `SHA256SUMS`, then extract `takaro-vein-plugin.tar.gz`. Verify `TakaroVein/SHA256SUMS` inside it. Keep the extracted licenses and `THIRD-PARTY.md` with the installation record.
3. Put `libtakaro-vein.so` outside Steam's installation tree, for example `data/vein-plugin/libtakaro-vein.so`. SteamCMD `validate` can remove unknown files from the game tree. Mount the plugin directory read-only at `/opt/takaro` if using containers.
4. Create a writable persistent connector directory, for example `data/vein-connector`, mounted at `/opt/takaro-state`. Copy `.env.example` and `docker-compose.example.yml` from the archive and set `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN` and the VEIN server settings. The example keeps `TAKARO_PLUGIN_DATA_DIR=/home/steam/vein/Vein/Binaries/Linux/takaro` inside the mounted game data, so the plugin's `bans.json` stays at its existing enforcement path. It sets `TAKARO_STATE_DIR=/opt/takaro-state/connector-state` for connector state. Keep both directories across restarts and upgrades.
5. Apply `LD_PRELOAD` to the **game binary only**, for example:

   ```bash
   LD_PRELOAD=/opt/takaro/libtakaro-vein.so ./Vein/Binaries/Linux/VeinServer-Linux-Test -Port=7777 -QueryPort=27015 -log
   ```

   Do not export it globally or pass it to SteamCMD, which is 32-bit. The server image in the Compose example must apply `TAKARO_PLUGIN_SO` only when launching the game binary.
6. Start the one game service. Confirm the game answers its own loopback `:8080/status`, the plugin is mapped in the game PID, and Takaro reports the same identity reachable. `VEIN_HTTP_API` defaults to the game's built-in read-only `http://127.0.0.1:8080` for player/name fallback; an explicit empty value disables that fallback. It is distinct from plugin diagnostics. If `TAKARO_PLUGIN_TOKEN` is set, authenticated loopback diagnostics are available at `127.0.0.1:18890/health`; the native connection does not use this token. An empty environment token falls back to `token` in `TAKARO_PLUGIN_DATA_DIR/plugin.json`; check that existing file during upgrades. The diagnostic listener is disabled only when neither supplies a token. The optional `TAKARO_CA_FILE` must point to a CA file readable **inside** the game container; an invalid file or certificate fails closed.

`TakaroVein/scripts/smoke-server.py` is an operator precheck against an already-started container. It checks the exact installed Steam server build, game status, native connection, queue and persistence fields, and the durable outbox. From the bundled Compose directory, run `./scripts/smoke-server.py --expected-build <server Steam build ID>`; its defaults are container `vein` and host manifest `data/vein/steamapps/appmanifest_2131400.acf`. Override them with `--game <container name> --manifest <host manifest path>` for other layouts. It needs `TAKARO_PLUGIN_TOKEN` in the container environment for diagnostics, even if the listener uses a file token. A successful precheck is not a substitute for real-client events and Takaro responses.

## Upgrade from the former sidecar

The old sidecar and the native connector must never use the same Takaro identity at once. Schedule a maintenance window, stop new player admissions, let connected players leave, and save the current state directory. The actual legacy files are `event-cursor.json`, `online-players.json`, `timed-bans.json` and `known-players.json`. Keep the plugin's game-enforcement `bans.json` separate at the **current** `TAKARO_PLUGIN_DATA_DIR` path; verify its current hash and contents before and after upgrade. Do not change the plugin data directory unless you explicitly transfer the current `bans.json` with its permissions and verify the destination before native startup. Set `TAKARO_STATE_DIR` to the legacy sidecar's mounted state directory or pass explicit `TAKARO_CURSOR_FILE`, `TAKARO_ONLINE_FILE`, `TAKARO_BAN_FILE` and `TAKARO_KNOWN_PLAYERS_FILE` pointing to those current files **inside** the game container. Explicit paths take precedence. A `.env` key is only applied if Compose forwards it to the game service; the bundled Compose example forwards naming, state and log override keys. Do not replace current bans with an older snapshot.

The old binary has no atomic game-side event admission barrier. Close new player ingress with a site-specific firewall or edge rule, wait for all players to leave, and prepare an executable `fence-check` script that exits zero only while that rule is active.

The ingress rule must remain effective when the game container is recreated. A rule installed only inside the old container's network namespace disappears with that namespace and does not protect the replacement. Use an ingress boundary that survives recreation, verify it against the replacement game before admitting players, and retain an independent timed restoration mechanism throughout the maintenance window. Record its rule and restoration command in a private JSON file such as `fence-proof.json`:

```json
{"method":"edge firewall rule blocking new VEIN UDP ingress","gamePort":7777,"expiresAtUtc":"<UTC within the next 30 minutes>","restoreCommand":"operator command that removes the rule"}
```

Replace the container placeholders and example port `7777` below with your actual deployment values, including the same port in `fence-proof.json`. Use a current expiry within 30 minutes and register a separate time-limited cleanup for the ingress rule before running the bundled helper while the old game and sidecar still run. The helper checks the rule; it does not execute `restoreCommand` or silently reopen ingress:

```bash
install -d -m 700 ./vein-migration-evidence
TakaroVein/scripts/compare-native-log-grammar.py \
  --sidecar-container "<legacy-sidecar-container>" \
  --probe TakaroVein/scripts/native-log-probe \
  --fixtures TakaroVein/scripts/native_log_legacy_fixtures.json \
  --report ./vein-migration-evidence/grammar-report.json
TakaroVein/scripts/drain-legacy.py --fence-proof ./fence-proof.json \
  --fence-check ./fence-check \
  --grammar-report ./vein-migration-evidence/grammar-report.json \
  --evidence-dir ./vein-migration-evidence \
  --game-container "<game-container>" --sidecar-container "<legacy-sidecar-container>" \
  --game-port 7777
```

The one-shot grammar check compares configured legacy JavaScript regex captures with pinned PCRE2 on legacy fixtures. It reports `none` when no custom expressions exist and names any incompatible `VEIN_LOG_*_RE` key. The drain checks that report against the **live** old sidecar configuration and exact fixture hash; a stale, failing or mismatched report blocks cutover. The probe is an operator tool; Node is needed only inside the already-running legacy sidecar for this one-time comparison and is not a native runtime dependency.

The drain helper checks the fence at each sample, requires the game's `/status.onlinePlayers` and plugin's live player list to be empty, and takes three stable five-second polls with pending and unconfirmed events at zero and matching sidecar cursor, scan cursor and ring sequence. VEIN's built-in `/players` can retain cached offline names, so the helper archives that response but uses `/status.onlinePlayers` for live admission. It stops the sidecar, checks the ring once more, then stops the game and removes the old sidecar container only if no delta appeared. It restarts the sidecar and refuses cutover if a post-stop ring delta appears while the game is still running. Inspect `drain-report.json` and the archived raw ring samples before installing the native library. The report deliberately states `exactBarrier: false`: an event between the final ring read and game shutdown cannot be excluded with the legacy binary. Treat any remaining or suspected events as undelivered and preserve the archived ring for reconciliation. Do not call this an exact drain. Remove the legacy sidecar service definition as well so an orchestrator cannot recreate it. Start only the native game process with the same Takaro identity, then confirm no orphan sidecar or helper and that the game PID owns the Takaro socket. Reopen ingress with the recorded restoration command only after the new game and Takaro identity are verified, and record the cleanup result.

The connector confirms event transmission after a later ping receives a pong on the same connection. Takaro has no per-event application acknowledgment, so duplicates can occur after a lost pong. Health reports queue sizes, delivery losses and persistence errors. A persistence error degrades connector health while the game keeps running; investigate it before another planned restart.

On the first native start after a sidecar upgrade, the connector imports legacy `timed-bans.json` expiry metadata into the current game-enforcement ban records once. Completion is recorded durably as `legacyBanMigrationDone` in `event-outbox.json`. On later native starts, current ban records are authoritative; a stale legacy timed-ban row must not turn a newer permanent ban back into a timed ban. Preserve the outbox and its migration marker across native restarts. If the marker cannot be written durably or legacy timed metadata conflicts with current enforcement, resolve the reported error before admitting players or claiming migration complete.

An interrupted ban or unban can leave `ban-intent.json` with the intended canonical ban state, including expiry and reason, while the game and plugin ban files disagree. The connector keeps that intent, reports `banRecoveryPending=true` and degraded health when it cannot prove the result. Inspect the current game ban, plugin `bans.json`, timed metadata and journal, then retry the **intended canonical ban or unban** through Takaro after resolving any ambiguity. Do not delete the journal to silence the warning, and do not copy an old ban file over current state.

## Rollback

For a planned rollback, stop admissions and drain the **current** native outbox first. Require `queues.outbox`, `queues.actions`, `queues.runningActions`, `queues.inbound` and `queues.outbound` to be zero, `outboxDurabilityPending=false`, `banRecoveryPending=false`, no active persistence error, and no unresolved `ban-intent.json` before restoring the legacy connector. The old sidecar cannot interpret a native ban intent. Save a copy of current state and record any undelivered entries.

After stopping the game, re-read the **stopped process's current** `event-outbox.json`, `ban-intent.json` if present, and compatible state files. Shutdown can admit new events after the last healthy, empty queue sample. Do not switch to the legacy sidecar, remove the outbox, or change state paths based only on the pre-stop drain. Preserve and reconcile newly admitted entries and any unresolved intent first; if native replay is needed, keep admissions closed and restart native against that same current state, then repeat the stopped-state check. An ordinary native-to-native update can retain and replay a nonempty compatible outbox on its next start. Once the planned rollback's stopped-state check is satisfied, restore the prior binary and configuration and restart the prior connector arrangement with the same Takaro identity only after the native process is gone. Retain current compatible state files and current ban changes. Never restore a stale `timed-bans.json` or game `bans.json` over newer files.

The native `event-outbox.json` also contains derived online and known-player state and the one-time legacy-ban migration marker. If the planned drain leaves its `pending` array empty **and** no ban intent or persistence error remains, archive that **current** file for audit and remove the active copy before a later native re-upgrade; this lets the new native process import online, known-player and timed-ban metadata from files updated by the restored sidecar. Verify those current files and game bans before re-upgrade. If an emergency rollback leaves pending entries or a ban intent is unresolved, preserve and report the **current** outbox and `ban-intent.json`. Reconcile pending event delivery, actual game-enforcement bans, native timed-ban metadata and the unresolved intent before any later re-upgrade resets migration provenance, and before enabling the legacy sidecar's timed-ban expiry. Do not overwrite or delete a nonempty outbox or unresolved journal to force a re-upgrade. Keep current timed bans and game-enforcement bans through both directions of the switch.

Record the candidate library SHA256, game/client versions, game PID and mapped library, socket owner, Takaro MCP responses and event UUIDs during verification. The known limitations are wire-only log events, incomplete entity/location coverage and whisper isolation requiring a second client.
