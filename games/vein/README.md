# Takaro VEIN Connector

A native library that runs inside the VEIN **Linux** dedicated server: you copy two files, with no
startup-line change. Players do not install anything.

## Install

You need: a VEIN **Linux** dedicated server you can upload files to (Windows servers are not
supported), and a Takaro game server of type **Generic** with its registration token.

### 1. Download

From https://takaro.io/connectors/vein, download `takaro-vein-plugin.tar.gz` and extract it (7-Zip
on Windows). Do not use the `vein-dev` or `pr-…` pre-releases.

### 2. Install

Stop the server. Upload `libtakaro-vein.so` and `libsteam.so` from the `TakaroVein` folder into
`Vein/Binaries/Linux`, the folder that holds `VeinServer-Linux-Test`:

- Pterodactyl / Pelican: `/home/container/Vein/Binaries/Linux`
- AMP: `vein/2131400/Vein/Binaries/Linux`
- Plain Linux: `<server folder>/Vein/Binaries/Linux`

If you control the start command, you can use
`LD_PRELOAD=/full/path/to/libtakaro-vein.so ./VeinServer.sh` instead of `libsteam.so`. Docker: see
[INSTALL.md](INSTALL.md).

### 3. Configure

Start the server; it creates `Vein/Binaries/Linux/takaro.cfg`. Paste your token and save; it
connects within a few seconds, no restart needed:

```ini
TAKARO_REGISTRATION_TOKEN=<your registration token>
TAKARO_IDENTITY_TOKEN=
TAKARO_SERVER_NAME=My VEIN server
```

Leave `TAKARO_IDENTITY_TOKEN` empty; the connector fills it in. Keep it once set.

| Optional setting | What it does |
|---|---|
| `TAKARO_ADMIN_STEAMIDS` | Comma-separated SteamID64s that get in-game admin rights (restart to apply). |
| `TAKARO_SERVER_CHAT_NAME` | Sender name on messages from Takaro, used when none is set for the server in Takaro (restart to apply). |

### 4. Check that it works

The console shows `[Takaro] connected to Takaro as "…"` and the server shows as online in Takaro.
Otherwise a block of `*` lines says what to fix. No `[Takaro]` line at all means the files are not
in the right folder.

**Upgrading:** replace `libtakaro-vein.so` and `libsteam.so`; `takaro.cfg` stays. From the v0.2.x
sidecar, follow [INSTALL.md](INSTALL.md).

## What works, what doesn't

Checked on VEIN 0.025h3 (server build 25969514), 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | |
| Player list | ✅ | SteamID64 and ping. |
| Player location | ✅ | None while on the character screen. |
| Teleport | ✅ | |
| Player inventory | ✅ | Once the player has spawned. |
| Item list | ⚠️ | Items removed by a game update stay in Takaro. |
| Entity list | ⚠️ | Types removed by a game update stay in Takaro. |
| Locations | ✅ | |
| World map | ⚠️ | Upload the map image once ([INSTALL.md](INSTALL.md#world-map)). Not checked yet. |
| Send a message | ✅ | Broadcasts in chat; whispers as a notification. |
| Console commands | ✅ | The connector's own set (`help` lists it). |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent, also offline. |
| Ban list | ✅ | |
| Give an item | ✅ | |
| Shop | ✅ | |
| Shut down the server | ✅ | Saves first. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | |
| Entity killed | ✅ | AI-on-AI kills are not reported. |
| Server log lines | ⚠️ | Takaro does not keep them. |
| Discord chat bridge | ✅ | Use `chatBridgeNoEcho`. |
| Modules | ✅ | |
| Game name, version and icon | ⚠️ | Not checked yet. |
| Restarts and outages | ✅ | Events are delivered after reconnecting. |
| Game updates | ✅ | A feature an update breaks reports degraded; the rest keeps working. |

## Known issues

- Proximity chat also reports as global.
- `AdminSteamIDs` in `Game.ini` is ignored by the game; use `TAKARO_ADMIN_STEAMIDS`.
- A timed ban shows as permanent in Takaro; the connector still lifts it on time.
- Characters do not survive a server restart (game behaviour).

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
