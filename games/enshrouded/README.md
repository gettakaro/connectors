# Takaro Enshrouded Connector

Connects an Enshrouded dedicated server (Windows, or Linux under Proton) to Takaro with one file:
`dbghelp.dll`, placed next to `enshrouded_server.exe`. Players do not install anything.

## Install

### 1. Before you start

- An Enshrouded dedicated server you can stop, start and copy files to, on a supported game build:
  **1273043** (Enshrouded 1.0, Steam build 26083076) or **1267041** (hotfix 1.0.0.1). Build 1024233
  is no longer supported.
- Windows, or Linux under Proton. With Docker, use `mornedhels/enshrouded-server:dev-proton`
  (GE-Proton11-9); the older GE-Proton10-30 image cannot start Enshrouded 1.0 (`No SteamUtils011`).
- The game files already in place. The connector does not install or update the game, and SteamCMD
  downloads only Steam's current build.
- A Takaro game server of type **Generic** and its registration token.

### 2. Download

Download the latest release from https://takaro.io/connectors/enshrouded (or the latest
`enshrouded-v<version>` release on https://github.com/gettakaro/connectors/releases):
`takaro-enshrouded-plugin-proton-1273043-<version>.zip`. The same `dbghelp.dll` runs on every
supported build, on Windows and under Proton. `takaro-enshrouded-plugin.zip` is the same file under
its old name. Do not use the `enshrouded-dev` or `pr-<number>-enshrouded` pre-releases.

Check the zip against `SHA256SUMS`:

```bash
sha256sum -c SHA256SUMS --ignore-missing
```

### 3. Install

Stop the game server (a running server holds `dbghelp.dll` open). The zip holds one folder,
`TakaroEnshrouded/`; copy its `dbghelp.dll` next to `enshrouded_server.exe`:

```text
<server folder>/
    enshrouded_server.exe
    dbghelp.dll                  <- from the zip
    takaro/
        plugin.json              <- created by the connector; your settings
```

Under Wine/Proton, set `WINEDLLOVERRIDES=dbghelp=n,b` on the game server so it loads this DLL. A
Windows server loads it already.

**With Docker**, use [`docker-compose.example.yml`](docker-compose.example.yml),
[`.env.example`](.env.example) and [`server/enshrouded-updater`](server/enshrouded-updater) (keep it
executable) from this folder. The compose file sets `WINEDLLOVERRIDES`, passes the Takaro settings
from `.env`, and mounts the DLL from `data/enshrouded/server/takaro/plugin/dbghelp.dll`. Copy your
Enshrouded server files (the folder with `enshrouded_server.exe`) into `data/enshrouded/server`
first; with an empty folder no game server runs.

```bash
mkdir -p data/enshrouded/server/takaro/plugin
cp TakaroEnshrouded/dbghelp.dll data/enshrouded/server/takaro/plugin/
cp .env.example .env    # fill in TAKARO_REGISTRATION_TOKEN and the role passwords
docker compose -f docker-compose.example.yml --env-file .env up -d
```

`server/enshrouded-updater` stops the image from updating the game at every container start. If you
host the server some other way, turn automatic game updates off there too.

### 4. Configure

Start the server. The connector creates `takaro/plugin.json` next to `enshrouded_server.exe`. Paste
your registration token into `registrationToken` and save: it connects within a few seconds, no
restart needed. To connect at the first start, create the file yourself first:

```json
{
  "registrationToken": "<your registration token>",
  "identityToken": "",
  "name": "",
  "url": "wss://connect.takaro.io/",
  "protocol": 1,
  "token": "",
  "mapDir": "",
  "caFile": "",
  "serverChatName": ""
}
```

| Key | What to put there |
|---|---|
| `registrationToken` | The registration token of your Takaro game server. Required. |
| `identityToken` | Leave empty; the connector writes the server's identity there. Keep it once set. |
| `name` | The server name in Takaro. Empty: your server's name plus the start of the identity. |
| `url` | Leave `wss://connect.takaro.io/`. |
| `protocol` | Leave `1`. `0` turns the world map off. |
| `token` | Optional: a long random secret to read diagnostics at `http://127.0.0.1:18890/health`. Needs a restart. |
| `mapDir` | Optional: folder of a map tile pack for an older Takaro. Empty: `takaro/map`. Needs a restart. |
| `caFile` | Optional: a CA file, only for a Takaro behind your own certificate authority. Needs a restart. |
| `serverChatName` | Optional sender name on messages from Takaro (see below). Applies within a few seconds. |

Changes to `registrationToken`, `identityToken`, `name`, `url` or `protocol` apply within a few
seconds. Instead of the file you can set `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`,
`TAKARO_SERVER_NAME`, `TAKARO_WS_URL` and `TAKARO_PLUGIN_TOKEN`; a set variable wins over the file
and needs a restart to change.

**World map.** The server has no map image of its own. Upload one image once on the game server's
map page in Takaro (see [INSTALL.md](INSTALL.md), "Map"); without it Takaro shows a grid with the
player dots.

**Server chat name (optional).** Messages from Takaro show in chat as a notice that starts with the sender name, `Takaro: hello`, never under a player's name. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `serverChatName` instead; the name set in Takaro wins over it. With neither, only the text shows.

### 5. Check that it works

Within about 20 seconds of the start (about a minute on the first start of a new Docker container),
the server console and `takaro/plugin.log` show:

```text
[Takaro] connecting to wss://connect.takaro.io/ as "<name>"
[Takaro] connected to Takaro as "<name>"; the server shows as reachable in the Takaro dashboard
```

and the game server shows as online in Takaro. Otherwise a block of `*` lines says what to fix:
`registrationToken not set` (token missing), `Takaro refused this server` (token wrong), or
`Takaro refused the server name` (another server in your Takaro already has that name).

`takaro/plugin.log` also names the game build (`game build: <n>|…`) and lists every feature as `ok`
or `degraded` in its `capabilities:` line. A `degraded` feature does not work on this build; an
unknown build logs `gamedata: no section for build <n>`.

### 6. Upgrade

Stop the server, replace only `dbghelp.dll`, and start it again. With the Docker example that is
`data/enshrouded/server/takaro/plugin/dbghelp.dll`, not the empty file Docker leaves next to
`enshrouded_server.exe`. Keep `takaro/plugin.json` and `takaro/connector-state/`. To pick up new
defaults in `takaro/enshrouded-gamedata.json`, delete that file.

Before the game updates to a new build, install a release made for it (its zip name carries the
build). Coming from 0.5.0 (plugin + sidecar): follow "Upgrade from 0.5.0" in
[INSTALL.md](INSTALL.md).

## What works, what doesn't

Checked on game build 1273043 (Enshrouded 1.0), 2026-10-21. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Encrypted connection; shows as reachable while the server runs. |
| Player list | ✅ | Character names, SteamID64 as id. Offline players seen before can be looked up. |
| Player location | ✅ | |
| Teleport | ✅ | To whole coordinates, exactly; pick a spot above ground. |
| Player inventory | ✅ | Matches the backpack. |
| Item list | ✅ | About 4,000 items with their English names; items the game never names are left out. |
| Entity list | ⚠️ | Bosses, NPCs and fish have their in-game names; other creatures get a name built from the internal code, for example "Rat". |
| Locations | ✅ | About 800 map places under their in-game names, including Flame Shrines and Fast Travel Points. |
| World map | ⚠️ | You upload the map image yourself. Not checked on takaro.io yet. Without an upload, a tile pack you render yourself also works (see [INSTALL.md](INSTALL.md)). |
| Send a message | ⚠️ | Broadcasts show in chat as a notice with the sender name in front, never under a player's name. Whispers reach the player; whether others also see them is not checked. |
| Console commands | ✅ | Enshrouded has no console; the connector provides `help`, `players`, `levels`, `level`, `say`, `whisper`, `location`, `teleport`/`tp`, `inventory`, `give`, `item`, `kick`, `save-and-shutdown`. `give <player> <itemCode> [amount] [level]`; `levels` and `level <player>` show character level and experience. |
| Kick | ✅ | The player must be online. |
| Ban and unban | ✅ | Ban needs the player online; unban works offline. The connector lifts timed bans itself when they end. |
| Ban list | ✅ | Timed bans show reason and end time; permanent bans show no reason. |
| Give an item | ✅ | Into the backpack, in stacks of at most 64. A quality of 1-255 sets the item level (the item's own level if it cannot have that one); other values are refused. |
| Shop | ✅ | In-game and Takaro orders, bundles and offline orders; not enough currency is refused; balances set in Takaro show in game. |
| Shut down the server | ✅ | Saves, then quits; your host or container restart policy starts it again. |
| Player joined / left | ✅ | Includes kicks, bans and shutdowns; a player online during a crash is reported as left on the next start. |
| Chat messages | ✅ | |
| Player death | ✅ | Falls and creature kills, with the position and the creature's name. |
| Entity killed | ⚠️ | Once per kill, with the weapon's name. Magic kills are not checked; destroyed props are not reported. |
| Server log lines | ⚠️ | Sent, but Takaro does not keep them. |
| Discord chat bridge | ⚠️ | Discord to game works; bot posts are ignored. Game chat and join/leave notices are posted to Discord but not confirmed in the channel. Use the `chatBridgeNoEcho` module. |
| Modules | ✅ | Chat commands, hooks, cronjobs, teleports and server messages. |
| Game name, version and icon | ✅ | Takaro shows Enshrouded with its icon and the running game build. |
| Restarts and outages | ✅ | Back within about 25 seconds after a restart or crash; notices a dead link within 20 seconds. Events are kept on disk until Takaro is reachable (one duplicate is possible). |
| Game updates | ✅ | A feature broken by a new game build reports itself as degraded; the rest keeps working. |

## Known issues

- Only builds 1273043 and 1267041 are supported. On any other build the item and creature lists and
  giving items switch off, and other features may too.
- Teleport lands exactly on the coordinates, so a spot in the ground or the air is unsafe.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
