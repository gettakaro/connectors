# Takaro VEIN Connector

A native connector library for the VEIN **Linux** dedicated server. It runs inside the game
server: you copy two files into the game's binaries folder, with no separate program, RCON or
startup-line change. Players do not install anything and keep the normal VEIN client.

## Install

### 1. Before you start

- A VEIN **Linux** dedicated server (Steam app 2131400) whose files you can upload to (panel file
  manager or SFTP). Windows dedicated servers and Workshop installation are not supported.
- Game client and server must run the same VEIN version.
- A Takaro game server of type **Generic** and its **registration token**. You fill in nothing
  else: the connector registers the server the first time it connects.

### 2. Download

Download `takaro-vein-plugin.tar.gz` from the latest release at https://takaro.io/connectors/vein
(or from the newest `vein-v…` release at
<https://github.com/gettakaro/connectors/releases?q=vein-v&expanded=true>; skip the `vein-dev`
and `pr-…` pre-releases). Optionally check it against the release's `SHA256SUMS`
(`sha256sum -c SHA256SUMS --ignore-missing`). Extract it (on Windows, 7-Zip opens `.tar.gz`);
you get a `TakaroVein` folder.

### 3. Install

Stop the server. Upload `libtakaro-vein.so` and `libsteam.so` from the `TakaroVein` folder into
the game's `Vein/Binaries/Linux` folder, the one that holds `VeinServer-Linux-Test`:

```
Vein/Binaries/Linux/
    VeinServer-Linux-Test
    libtakaro-vein.so
    libsteam.so
    takaro.cfg          (created by the connector on first start)
```

- Pterodactyl / Pelican: `/home/container/Vein/Binaries/Linux`
- AMP: `vein/2131400/Vein/Binaries/Linux` in the instance
- Plain Linux: `<server folder>/Vein/Binaries/Linux`

This works with every panel: while Steam starts, the game looks for an optional `libsteam.so` in
that folder, and the small loader with that name starts the connector. Steam itself is
unaffected, and a SteamCMD update or validate leaves the files in place. To remove the connector,
delete `libsteam.so`. If your host does not let you upload `.so` files, ask their support to
place the two files for you.

**Alternative: `LD_PRELOAD`.** If you control the start command, you can preload the connector
from any folder instead:
`LD_PRELOAD=/full/path/to/libtakaro-vein.so ./VeinServer.sh -Port=7777 -QueryPort=27015`.
`takaro.cfg` is then created next to the library (`TAKARO_CONFIG_FILE` sets another path). The
config keys also work as environment variables, which win over the file, are never written to it
and need a restart when changed. Do not also install `libsteam.so` (if both are present the
connector still starts only once). Docker: see `docker-compose.example.yml` and `.env.example` in
the archive, and [INSTALL.md](INSTALL.md).

### 4. Configure

Start the server. The connector creates `Vein/Binaries/Linux/takaro.cfg` and prints a block of
`*` lines naming it. Paste your token after `TAKARO_REGISTRATION_TOKEN=` and save. No restart is
needed: the connector connects within a few seconds, also after you correct a token.

The whole file, if you prefer to create it before the first start (leave `TAKARO_IDENTITY_TOKEN`
empty; the connector writes a new one into it):

```ini
TAKARO_REGISTRATION_TOKEN=<your registration token>
TAKARO_IDENTITY_TOKEN=
TAKARO_SERVER_NAME=My VEIN server
TAKARO_ADMIN_STEAMIDS=
TAKARO_PROTOCOL=1
```

- `TAKARO_IDENTITY_TOKEN` identifies this server in Takaro; keep it when you move the server.
- `TAKARO_SERVER_NAME` is the name Takaro shows. A file the connector creates uses
  `My VEIN server (<first 8 characters of the identity>)`.
- `TAKARO_ADMIN_STEAMIDS` (optional, comma separated SteamID64s) get VEIN's in-game admin rights.
- `TAKARO_PROTOCOL`: leave `1`. `0` turns off the world map and game info in Takaro.

A saved change to the token, identity, name, URL or `TAKARO_PROTOCOL` reconnects within a few
seconds; `TAKARO_ADMIN_STEAMIDS` and every other key need a restart.

**Server chat name (optional).** Messages from Takaro show as `[Name] message`; VEIN has no sender field for server messages. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `TAKARO_SERVER_CHAT_NAME=Your Name` in `takaro.cfg` instead and restart; the name set in Takaro wins over it. With neither, the name is `TAKARO_SERVER_NAME`, else `Server`. The older key `TAKARO_SENDER_NAME` still works and wins over `TAKARO_SERVER_CHAT_NAME`.

### 5. Check that it works

The server console shows `[Takaro]` lines; `connected to Takaro as "…"` means it works, and the
game server shows as reachable in Takaro. To test it in game, install the **utils** module and
type `@ping` in chat: Takaro answers `Pong!`. Until you set `TAKARO_ADMIN_STEAMIDS`, Takaro's
reachability check says `adminGrant=degraded`; that is expected and harmless.

A block of `*` lines tells you what to fix and names the file to edit:

- `TAKARO_REGISTRATION_TOKEN not set`: the token is missing.
- `Takaro refused this server`: the token is wrong.
- `no trusted CA bundle found`: the server has no CA certificates; install the `ca-certificates`
  package or set `TAKARO_CA_FILE`.

No `[Takaro]` line at all means the connector was not loaded: check the file names and folder.
The connector's own log is `Vein/Binaries/Linux/takaro/plugin.log`.

### 6. Upgrade

Stop the server, upload the new `libtakaro-vein.so` and `libsteam.so` over the old ones, and
start it. Replace only these two files; `takaro.cfg` is never touched. Upgrading from the old
sidecar release (v0.2.x) needs a short maintenance window: follow [INSTALL.md](INSTALL.md).

## What works, what doesn't

Checked on VEIN 0.025h3 (server build 25969514), 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | The server shows as reachable while the game runs. |
| Player list | ✅ | Name, ping, spawned state and SteamID64; one-player lookup also answers for offline players. |
| Player location | ✅ | Live position, following walking and teleports; none while the player is on the character screen. |
| Teleport | ✅ | |
| Player inventory | ✅ | Matches the in-game bag; unavailable until the player has a spawned character. |
| Item list | ⚠️ | Every item, with in-game display names; items renamed or removed by a game update stay in Takaro. |
| Entity list | ⚠️ | Creature types sync; types renamed or removed by a game update stay in Takaro. |
| Locations | ✅ | The game's named places, with their in-game names. |
| World map | ⚠️ | The server files have no map picture: upload the in-game map once in the Takaro dashboard ([INSTALL.md](INSTALL.md#world-map)). Not checked on takaro.io yet. |
| Send a message | ✅ | A broadcast is a chat line `[Name] message`; a whisper is an on-screen notification `[Name] message`. |
| Console commands | ✅ | The connector's own set: `help`, `players`, `say`, `give`, `tp`, `ban`, `save`, … |
| Kick | ✅ | The player can rejoin afterwards. |
| Ban and unban | ✅ | Timed and permanent, also for offline players; timed bans are lifted at expiry, also across a restart. |
| Ban list | ✅ | The game's bans plus the connector's, with reason and expiry. |
| Give an item | ✅ | Appears in the bag without a relog. |
| Shop | ✅ | In-game and dashboard orders, bundles, offline orders and `@balance` work; a buy without enough currency is refused. |
| Shut down the server | ✅ | Saves the world first, then quits cleanly. |
| Player joined / left | ✅ | Left also arrives after a crash. |
| Chat messages | ✅ | The connector's own messages are not echoed back. |
| Player death | ✅ | Position and cause; falls and drowning have no attacker. |
| Entity killed | ✅ | Creature, player and held item. AI-on-AI kills are not reported. |
| Server log lines | ⚠️ | Sent with secrets (join password, Steam tickets) redacted, but Takaro does not store them. |
| Discord chat bridge | ✅ | Both ways; bot posts are ignored. Use the `chatBridgeNoEcho` module, since stock `chatBridge` re-posts server messages. |
| Modules | ✅ | Chat commands, hooks, cronjobs, teleports, server messages and onboarding. |
| Game name, version and icon | ⚠️ | Not checked on takaro.io yet. |
| Restarts and outages | ✅ | Re-identifies after a restart or network drop; events are kept on disk and sent once (rarely one duplicate). |
| Game updates | ✅ | A feature broken by a game update reports `degraded`; the rest keeps working. |

## Known issues

- The two-file install relies on the game looking for an optional `libsteam.so` while Steam
  starts. If an update stops that, no `[Takaro]` line appears; please report it and use
  `LD_PRELOAD` meanwhile.
- Local and global chat cannot be told apart: proximity chat also reports as global.
- `AdminSteamIDs` in `Game.ini` is ignored by the game. Use `TAKARO_ADMIN_STEAMIDS` instead.
- A timed ban shows as permanent in Takaro; the connector still lifts it on time.
- Characters do not survive a server restart. This is the game's own behaviour; warn your players.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
