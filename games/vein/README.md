# Takaro VEIN Connector

The Linux dedicated server loads one native `libtakaro-vein.so` connector. It connects directly to Takaro; players install nothing and keep the vanilla VEIN client. The library must be preloaded on the game binary's launch line. Windows and Workshop installation are not supported.

## Install

Runs on a VEIN **Linux** dedicated server (Steam app 2131400) only; Windows servers cannot run it. Players install nothing and keep the normal VEIN client. Game client and server must run the same VEIN version.

### 1. Get your registration token from Takaro

In the Takaro dashboard, add a game server of type **Generic** and copy the **registration token** it shows. You do not fill in anything else: the connector registers the server the first time it connects.

### 2. Put three files next to the game

Download `takaro-vein-plugin.tar.gz` from the newest `vein-v…` [release](https://github.com/gettakaro/connectors/releases?q=vein-v&expanded=true) and extract it (on Windows, 7-Zip opens `.tar.gz`). From the `TakaroVein` folder:

1. Rename `takaro.cfg.example` to `takaro.cfg`, open it and put your token after `TAKARO_REGISTRATION_TOKEN=`. Optionally set `TAKARO_SERVER_NAME`.
2. Stop the server.
3. With your panel's file manager or SFTP, upload `libtakaro-vein.so`, `libsteam.so` and `takaro.cfg` into the game's `Vein/Binaries/Linux` folder, the one that holds `VeinServer-Linux-Test`:
   - Pterodactyl / Pelican: `/home/container/Vein/Binaries/Linux`
   - AMP: `vein/2131400/Vein/Binaries/Linux` in the instance
   - Plain Linux: `<server folder>/Vein/Binaries/Linux`
4. Start the server.

This works with every panel and needs no startup-line or setting change: while Steam starts, the game looks for an optional `libsteam.so` in that folder, and the small loader with that name starts the connector next to it. Steam itself is unaffected. A SteamCMD update or validate leaves the three files in place. To update the connector, stop the server and upload the new `libtakaro-vein.so` and `libsteam.so`; `takaro.cfg` stays. To remove it, delete `libsteam.so`.

If your host does not let you upload `.so` files into the game folder, ask their support to place the three files for you.

### 3. Check it works

The server console shows `[Takaro]` lines; `connected to Takaro as "…"` means it works, and Takaro lists the server as reachable. Install the **utils** module on the server in Takaro and type `@ping` in game chat: Takaro answers `Pong!`. `Takaro refused this server` means the token is wrong; `no registration token set` means it is missing; `no trusted CA bundle found` means the server has no CA certificates (install the `ca-certificates` package or set `TAKARO_CA_FILE`); no `[Takaro]` line at all means the connector was not loaded (check file names and folder). The connector's own log is `Vein/Binaries/Linux/takaro/plugin.log`.

### Advanced: LD_PRELOAD

Admins who control the start command can preload the connector instead, from any folder: `LD_PRELOAD=/full/path/to/libtakaro-vein.so ./VeinServer.sh -Port=7777 -QueryPort=27015`, with `takaro.cfg` next to the library (or the same settings as environment variables, which win over the file). Do not also install `libsteam.so`; if both are present the connector still starts only once. Docker users: see `docker-compose.example.yml` and `.env.example` in the archive; [INSTALL.md](INSTALL.md) covers state directories, upgrades from v0.2.x and rollback.

## What works, what doesn't

Verified on the native connector (v0.3.0) with a real game client and Takaro on VEIN 0.025 Hotfix 1 (server build 25581145), 2026-09-29. ✅ = works, ⚠️ = works with a caveat, ❌ = unsupported.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The plugin holds the Takaro connection itself; the server shows as reachable while the game runs. |
| Player list | ✅ | Name, ping and spawned state; `gameId` is the SteamID64. |
| Single player lookup | ✅ | Same data for one player; offline players answer with a last-known record. |
| Player location | ✅ | The live position, following walking and teleports. |
| Player inventory | ✅ | Matches the in-game bag; unavailable (404) without a character pawn, including before respawn. |
| Give an item | ✅ | The item appears in the bag without a relog. |
| Item catalogue | ✅ | Every loaded item class, with the in-game display names. |
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
| Shop: order while offline, claim later | ✅ | The order is paid while offline and delivered when claimed after rejoining. |
| Shop: not enough currency | ✅ | The purchase is refused and the balance is unchanged. |
| Economy: currency | ✅ | Balances are set, read and debited by Takaro. |
| Economy: balance in game | ✅ | `@balance` answers in chat. |
| Discord: game chat → Discord | ✅ | In-game chat is relayed to the linked channel. |
| Discord: Discord → game chat | ✅ | Real human posts are relayed into game chat; bot posts are ignored. |
| Discord: module hook / cronjob posts | ✅ | Hooks and cronjobs post to Discord and edit their messages. |
| Discord: join/leave notices | ✅ | Posted to Discord by the chat-bridge module. |
| Discord: no echo of server messages | ✅ | Stock `chatBridge` re-posts server messages; the `chatBridgeNoEcho` fork does not. |
| Events while the Takaro connection is down | ✅ | Kept on disk and delivered once the connection is back (tested with a 2-minute outage). |
| Reconnects after a server or container restart | ✅ | Re-identifies on its own after a restart, a crash or a container restart. |
| No duplicate events after a connector restart | ✅ | Unconfirmed events are replayed once from the on-disk outbox; a lost confirmation can rarely cause one duplicate. |
| Survives a network drop to Takaro | ✅ | The WebSocket reconnects by itself and re-identifies. |
| Timed bans expire on their own | ✅ | Lifted at expiry, including across a restart. |
| Keeps running after a game update breaks a feature | ✅ | The broken feature reports `degraded`; everything else keeps working. |

### Known issues

- The three-file install relies on the game's `libsteam_api.so` looking for an optional `libsteam.so` while Steam starts. If a game or Steam update stops doing that, no `[Takaro]` line appears; please report it, and use `LD_PRELOAD` meanwhile.
- Upgrading from the old sidecar release (v0.2.x) needs a short maintenance window; follow the [upgrade instructions](INSTALL.md).
- A broadcast shows as a chat line prefixed with the server name; a whisper as an on-screen notification.
- Local and global chat cannot be told apart — proximity chat also reports as global.
- `AdminSteamIDs` in `Game.ini` is ignored by the game. Use `TAKARO_ADMIN_STEAMIDS` instead.
- The server log contains the join password and Steam tickets; the connector redacts them from Takaro.
- Renamed or removed items and creatures linger in Takaro's catalogue, because its sync never deletes.
- A timed ban shows as permanent in Takaro until gettakaro/takaro#3981; it is still lifted on time.
- Characters do not survive a server restart. This is the game's own behaviour — warn your players.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
