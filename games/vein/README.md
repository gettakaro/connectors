# Takaro VEIN Connector

The Linux dedicated server loads one native `libtakaro-vein.so` connector. It connects directly to Takaro; players install nothing and keep the vanilla VEIN client. The library must be preloaded on the game binary's launch line. Windows and Workshop installation are not supported.

## Install

### 1. Prepare the server

Use a VEIN **Linux dedicated server** (Steam app 2131400) whose launch command you can change, and create a Takaro **Generic** game server with a registration token. The game client and server must run the same VEIN version. Players do not install anything.

### 2. Download and copy

Download `takaro-vein-plugin.tar.gz` and `SHA256SUMS` from the VEIN release at <https://takaro.io/connectors/vein>. Verify the archive with `sha256sum -c SHA256SUMS`, then extract it. Put `TakaroVein/libtakaro-vein.so` outside Steam's installation tree, for example `data/vein-plugin/libtakaro-vein.so`; SteamCMD validation removes unknown files from its tree. The archive also contains the example environment and Compose files, licenses, and [upgrade and rollback instructions](INSTALL.md).

### 3. Configure and start

Set `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN` and `TAKARO_WS_URL` in the **game process**. Keep `TAKARO_PLUGIN_DATA_DIR` at the existing writable game-data path so the current enforcement `bans.json` is retained; use persistent `TAKARO_STATE_DIR` for the connector files. Its default is `connector-state` under `TAKARO_PLUGIN_DATA_DIR`. Apply `LD_PRELOAD=/opt/takaro/libtakaro-vein.so` only to `VeinServer-Linux-Test`, never to SteamCMD. The included Compose example mounts the library read-only at `/opt/takaro` and connector state at `/opt/takaro-state`. Start only the game service. `TAKARO_CA_FILE` can point to a custom trusted CA file; certificate and hostname verification always remain on.

### 4. Verify

Confirm the game answers `127.0.0.1:8080/status`, Takaro shows the same identity reachable, and the game PID maps `libtakaro-vein.so`. Authenticated loopback diagnostics at `127.0.0.1:18890/health` use `TAKARO_PLUGIN_TOKEN`, falling back to the `token` in `TAKARO_PLUGIN_DATA_DIR/plugin.json`. The listener is disabled only when neither supplies a token; direct Takaro communication needs neither.

For the bundled Compose layout, run `./scripts/smoke-server.py --expected-build <server Steam build ID>` from the directory containing the Compose file on the Docker host. For another layout, also pass `--game <container name> --manifest <host path to appmanifest_2131400.acf>`. The smoke tool requires `TAKARO_PLUGIN_TOKEN` in the container environment, and checks the native connection, durable outbox and queue health. Pair a real client action and event with Takaro responses before treating the installation as verified.

## What works, what doesn't

The table below records the **previous sidecar release's** real-client verification on 2026-09-17 against game build 25035268 (v0.024h8). It is a compatibility reference, not a claim that every row has passed on the native candidate. Native rows remain pending until the full PC-client and Takaro MCP campaign is recorded. ✅ = previously worked, ⚠️ = previous caveat, ❌ = unsupported.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The server shows as reachable while the sidecar is up. |
| Player list | ✅ | Name, ping and spawned state; `gameId` is the SteamID64. |
| Single player lookup | ✅ | Same data for one player; offline players answer with a last-known record. |
| Player location | ✅ | The live position, following walking and teleports. |
| Player inventory | ✅ | Matches the in-game bag; unavailable (404) without a character pawn, including before respawn. |
| Give an item | ✅ | The item appears in the bag without a relog. |
| Item catalogue | ✅ | Every loaded item class. Readable names need `TAKARO_ITEM_NAMES=1`. |
| Entity catalogue | ⚠️ | Creature types sync, but Takaro never deletes rows from older builds. |
| Locations / points of interest | ⚠️ | Served by the connector, but Takaro never asks for them. |
| Run a console command | ✅ | The connector's own set: `help`, `players`, `say`, `give`, `tp`, `ban`, `save`, … |
| Broadcast a message | ✅ | Everyone sees it, as a chat line prefixed with the server name. |
| Whisper a player | ✅ | Reaches the one player, as an on-screen notification. |
| Teleport a player | ✅ | The player is moved to the requested position. |
| Kick a player | ✅ | The player is dropped and can rejoin afterwards. |
| Ban a player (timed and permanent) | ✅ | Works offline too; the connector lifts timed bans at expiry. |
| Unban a player | ✅ | The player can rejoin at once. |
| Ban list | ✅ | The game's bans plus the connector's, with reason and expiry. |
| Shut the server down | ✅ | Saves the world first, then quits cleanly. |
| Player joined event | ✅ | Arrives on every join, with the SteamID64. |
| Player left event | ✅ | Arrives on a clean quit and after a crash. |
| Player chat event | ✅ | Real player chat reaches Takaro; the connector's own messages are not echoed. |
| Player death event | ✅ | Position and cause included; falls and drowning have no attacker. |
| Entity kill event | ✅ | Creature, player and held item. AI-on-AI kills are not reported. |
| Log events | ⚠️ | Forwarded with secrets redacted, but Takaro does not store log events. |
| Map info | ❌ | Takaro does not support map info for Generic game servers. |
| Map tiles | ❌ | Takaro does not support map tiles for Generic game servers. |
| Modules: chat commands | ✅ | In-game commands reach the module and answer in chat. |
| Modules: hooks | ✅ | Chat and join hooks fire and run their code. |
| Modules: cronjobs | ✅ | Scheduled module runs fire and can message the server. |
| Modules: teleports (`@settp`, `@tp`, …) | ✅ | The teleports module's in-game commands move the player. |
| Modules: server messages / onboarding | ✅ | Timed messages and the welcome message are delivered in game. |
| Shop: buy in game | ✅ | Buying with the in-game chat command works. |
| Shop: order in Takaro and claim in game | ✅ | The items are delivered to the player. |
| Shop: bundle of several items | ✅ | One claim delivers every item in the listing. |
| Shop: order while offline, claim later | ✅ | Refused while offline, succeeds after rejoining. |
| Shop: not enough currency | ✅ | The purchase is refused and the balance is unchanged. |
| Economy: currency | ✅ | Balances are set, read and debited by Takaro. |
| Economy: balance / top list in game | ✅ | The in-game economy commands answer in chat. |
| Discord: game chat → Discord | ✅ | In-game chat is relayed to the linked channel. |
| Discord: Discord → game chat | ⚠️ | Works, but only a real human post can confirm it — bots are ignored. |
| Discord: module hook / cronjob posts | ✅ | Hooks and cronjobs post to Discord and edit their messages. |
| Discord: join/leave notices | ✅ | Posted to Discord by the chat-bridge module. |
| Discord: no echo of server messages | ✅ | Stock `chatBridge` re-posts server messages; the `chatBridgeNoEcho` fork does not. |
| Events while the Takaro connection is down | ✅ | Kept and delivered in order once the connection is back. |
| Reconnects after a server or container restart | ✅ | Re-identifies on its own; keep the sidecar's restart policy on. |
| No duplicate events after a connector restart | ✅ | The event cursor is persisted, so nothing is replayed. |
| Survives a network drop to Takaro | ✅ | The WebSocket reconnects by itself and re-identifies. |
| Timed bans expire on their own | ✅ | Lifted at expiry, including across a restart. |
| Keeps running after a game update breaks a feature | ✅ | The broken feature reports `degraded`; everything else keeps working. |

### Known issues

- Discord → game chat can only be confirmed by a real human message; the bridge ignores bots.
- A broadcast shows as a chat line prefixed with the server name; a whisper as an on-screen notification.
- Local and global chat cannot be told apart — proximity chat also reports as global.
- `AdminSteamIDs` in `Game.ini` is ignored by the game. Use `TAKARO_ADMIN_STEAMIDS` instead.
- The server log contains the join password and Steam tickets; the connector redacts them from Takaro.
- Renamed or removed items and creatures linger in Takaro's catalogue, because its sync never deletes.
- A timed ban shows as permanent in Takaro until gettakaro/takaro#3981; it is still lifted on time.
- Characters do not survive a server restart. This is the game's own behaviour — warn your players.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
