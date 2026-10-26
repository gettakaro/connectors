# Takaro VEIN Connector

A native connector that runs inside the VEIN Linux dedicated server and connects it to Takaro by
itself: you copy two files into the game folder, with no separate program, RCON or start-line
change. Windows dedicated servers are not supported. Players do not install anything.

## Install

### 1. Get a registration token

You need a VEIN **Linux** dedicated server (Steam app 2131400) on build 25969514 (0.025h3). Client
and server must run the same VEIN version. In Takaro, add a game server of type **Generic** and copy
its registration token.

### 2. Download

From <https://takaro.io/connectors/vein> or the latest `vein-v*` release on
<https://github.com/gettakaro/connectors/releases>, download `takaro-vein-plugin.tar.gz` and
extract it (on Windows, 7-Zip opens it). You get a `TakaroVein` folder.

### 3. Copy two files

Stop the server. Upload `libtakaro-vein.so` and `libsteam.so` from `TakaroVein` into the folder
that holds `VeinServer-Linux-Test`:

- Pterodactyl / Pelican: `/home/container/Vein/Binaries/Linux`
- AMP: `vein/2131400/Vein/Binaries/Linux` in the instance
- Plain Linux: `<server folder>/Vein/Binaries/Linux`

The game loads `libsteam.so` while Steam starts, and that loads the connector. SteamCMD updates
leave both files in place. If your host does not allow uploading `.so` files, ask their support.

### 4. Set the token

Start the server. The connector creates `takaro.cfg` in the same folder. Paste your token after
`TAKARO_REGISTRATION_TOKEN=` and save; it connects within a few seconds, no restart needed.

```ini
TAKARO_REGISTRATION_TOKEN=<your registration token>
TAKARO_IDENTITY_TOKEN=
TAKARO_SERVER_NAME=My VEIN server
TAKARO_ADMIN_STEAMIDS=
TAKARO_PROTOCOL=1
```

| Key | What to put there |
|---|---|
| `TAKARO_IDENTITY_TOKEN` | Leave empty; the connector fills it in. Keep it when you move the server. |
| `TAKARO_SERVER_NAME` | The name Takaro shows. |
| `TAKARO_ADMIN_STEAMIDS` | Optional: comma-separated SteamID64s that get in-game admin rights. Needs a restart. |
| `TAKARO_PROTOCOL` | Leave `1`; `0` limits the connector to Takaro protocol 0. |

### 5. Check that it worked

The server console shows `[Takaro] connected to Takaro as "…"` and Takaro lists the server as
reachable. Install the **utils** module and type `@ping` in game chat: Takaro answers `Pong!`.

If something is wrong, a block of `*` lines says what to fix: `TAKARO_REGISTRATION_TOKEN not set`
(token missing), `Takaro refused this server` (token wrong), or `no trusted CA bundle found`
(install the `ca-certificates` package). No `[Takaro]` line at all means the files are in the wrong
folder. The connector's log is `Vein/Binaries/Linux/takaro/plugin.log`.

`adminGrant=degraded` in Takaro's reachability check is harmless until you set `TAKARO_ADMIN_STEAMIDS`.

### Update

Stop the server, upload the new `libtakaro-vein.so` and `libsteam.so` over the old ones, and start
it. `takaro.cfg` is kept. Coming from the v0.2.x sidecar, or running in Docker with `LD_PRELOAD`:
see [INSTALL.md](INSTALL.md). To remove the connector, delete `libsteam.so`.

### Map (optional)

Player dots show on a grid. For a background, upload the game's in-game map once in the Takaro
dashboard (game server, Map, Upload), uncropped. Which image: [INSTALL.md](INSTALL.md#world-map).

## What works, what doesn't

Tested on server build 25969514 (VEIN 0.025h3) on 2026-10-26.

| Feature | Status | Note |
|---|---|---|
| Connection and reconnect | ✅ | Survives restarts, crashes and network drops. |
| Player list and lookup | ✅ | Name, SteamID64 and ping; offline players keep their last record. |
| Player location | ✅ | None while the player is on the character screen. |
| Player inventory | ✅ | Unavailable while the player has no character, e.g. dead. |
| Give item | ✅ | No relog needed. |
| Item catalogue | ✅ | In-game names; all schematics are named "Schematic". |
| Entity catalogue | ⚠️ | Removed creatures stay in Takaro's list. |
| Locations | ✅ | The game's named places, shown on the Takaro map. |
| Console commands | ✅ | Connector command set: `help`, `players`, `say`, `give`, `tp`, … |
| Broadcast and whisper | ✅ | Broadcast in chat; whisper as an on-screen notification. |
| Teleport | ✅ | |
| Kick | ✅ | The player can rejoin afterwards. |
| Ban, unban, ban list | ✅ | Timed and permanent, also offline; banned players cannot rejoin. |
| Shutdown | ✅ | Requests a save, then exits cleanly. |
| Join, leave and chat events | ✅ | |
| Death events | ✅ | Cause and position; falls have no attacker. |
| Entity kill events | ✅ | Creature, player and held item; AI-on-AI kills not reported. |
| Log events | ⚠️ | Secrets redacted; Takaro does not store log events. |
| Map | ⚠️ | Needs Takaro protocol 1. |
| Game name, version and icon | ⚠️ | Needs connector v0.8.0+ and Takaro protocol 1. |
| Modules (commands, hooks, cronjobs, teleports) | ✅ | |
| Shop and economy | ✅ | Bundles and offline orders included. |
| Discord chat both ways | ✅ | Use the `chatBridgeNoEcho` module. |
| Events during a Takaro outage | ✅ | Kept on disk and delivered once reconnected. |
| Whisper hidden from other players | ⚠️ | Not tested with two players. |

## Known issues

- If a game update stops loading `libsteam.so`, no `[Takaro]` line appears; use `LD_PRELOAD` (see INSTALL.md).
- Characters do not survive a server restart; this is the game's own behaviour.
- Local and global chat both report as global.
- `AdminSteamIDs` in `Game.ini` is ignored by the game; use `TAKARO_ADMIN_STEAMIDS`.
- A timed ban shows as permanent in Takaro, but is still lifted on time.
