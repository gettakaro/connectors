# Takaro Enshrouded Connector

Connects an Enshrouded dedicated server to Takaro with one file, `dbghelp.dll`, placed next to
`enshrouded_server.exe`. It runs on Windows and under Proton on Linux, on Enshrouded 1.0 (game build
1273043). Players do not install anything.

## Install

### 1. Before you start

- An **Enshrouded dedicated server** on game build 1273043, already installed. The connector does
  not install or update the game. Turn automatic game updates off: a new build can switch features
  off until a matching connector release is out.
- A Takaro game server of type **Generic** and its **registration token**.

Release 0.8.1 and older do not work on build 1273043. Use a release whose zip name contains
`proton-1273043`.

### 2. Download

Download **`takaro-enshrouded-plugin-proton-1273043-<version>.zip`** from
<https://takaro.io/connectors/enshrouded> or the latest `enshrouded-vX.Y.Z` release on
<https://github.com/gettakaro/connectors/releases>. The same DLL works on Windows. Do not use the
`enshrouded-dev` or `pr-…` pre-releases.

### 3. Copy it into place

Stop the server. Copy `dbghelp.dll` from the zip's `TakaroEnshrouded/` folder next to
`enshrouded_server.exe`.

Under **Wine/Proton**, set `WINEDLLOVERRIDES=dbghelp=n,b` on the server so it loads this DLL. Tested
with GE-Proton11-9 (`mornedhels/enshrouded-server:dev-proton`); GE-Proton10-30 cannot start the 1.0
server. For Docker, use [`docker-compose.example.yml`](docker-compose.example.yml),
[`.env.example`](.env.example) and [`server/enshrouded-updater`](server/enshrouded-updater) from this
folder. The DLL then goes in `data/enshrouded/server/takaro/plugin/dbghelp.dll`, the game files in
`data/enshrouded/server`, and the token in `.env`.

### 4. Set the registration token

Start the server. The connector creates `takaro/plugin.json` next to `enshrouded_server.exe`. Paste
your token into `registrationToken` and save: it connects within a few seconds, no restart needed.
To connect on the first start, create the file yourself:

```json
{
  "registrationToken": "<your registration token>",
  "identityToken": "",
  "name": "",
  "url": "wss://connect.takaro.io/",
  "protocol": 1,
  "token": "",
  "mapDir": "",
  "caFile": ""
}
```

| Key | What to put there |
|---|---|
| `registrationToken` | Your Takaro registration token. Required. |
| `identityToken` | Leave empty; the connector fills it in. Keep it once set. |
| `name` | Server name in Takaro. Empty: your server name plus a short id. |

Leave the other keys as they are. Environment variables `TAKARO_REGISTRATION_TOKEN`,
`TAKARO_IDENTITY_TOKEN`, `TAKARO_SERVER_NAME` and `TAKARO_WS_URL` override the file (restart needed).

### 5. Check that it worked

Within about 20 seconds the console and `takaro/plugin.log` show:

```
[Takaro] connected to Takaro as "<name>"; the server shows as reachable in the Takaro dashboard
```

The server is then **online** in Takaro. A block of `*` lines says what to fix: `registrationToken
not set`, `Takaro refused this server` (wrong token) or `Takaro refused the server name` (name
taken). The `capabilities:` line in `plugin.log` lists every feature as `ok` or `degraded`.

### 6. Upgrading

Stop the server, replace only `dbghelp.dll`, start it again. With Docker that is
`data/enshrouded/server/takaro/plugin/dbghelp.dll`. `takaro/plugin.json` is kept. Also delete
`takaro/enshrouded-gamedata.json` so the new release's game data is used (needed for character
levels). From 0.5.0 (plugin + sidecar): follow [INSTALL.md](INSTALL.md).

## What works, what doesn't

Tested on Enshrouded 1.0, game build 1273043, on 2026-10-25.

| Feature | Status | Note |
|---|---|---|
| Connection, restart, reconnect | ✅ | Also survives network drops and Takaro outages. |
| Game name, build and icon | ✅ | |
| Player list and lookup | ✅ | Character name and Steam ID; lookup works offline too. |
| Player location | ✅ | |
| Player inventory | ✅ | |
| Give an item | ✅ | Split into stacks of at most 64. |
| Item catalogue | ✅ | In-game names; a few non-items such as eye colours. |
| Entity catalogue | ⚠️ | Common creatures get names built from internal codes. |
| Locations | ✅ | In-game map names, e.g. Rookmore, Braelyn Bridge. |
| Run a console command | ✅ | Connector's own commands; `help` lists them, including `levels`. |
| Broadcast and whisper | ⚠️ | Whisper not tested with two players. |
| Teleport a player | ✅ | Lands exactly on the coordinates; pick a spot above ground. |
| Kick, ban, unban, ban list | ✅ | Kick and ban need the player online. |
| Timed bans | ✅ | Lifted by the connector when they expire. |
| Shut the server down | ✅ | Saves first. |
| Join, leave, chat, death events | ✅ | |
| Entity kill event | ⚠️ | Common creatures named from internal codes. |
| Log events | ⚠️ | Sent, but Takaro does not store them. |
| World map | ⚠️ | Needs protocol 1 and an uploaded image; see [INSTALL.md](INSTALL.md#map). |
| Modules: commands, hooks, cronjobs, teleports | ✅ | |
| Shop and economy | ✅ | |
| Discord: game chat to Discord | ⚠️ | Takaro posts it; not checked in the Discord app. |
| Discord: Discord to game chat | ✅ | |

## Known issues

- A game update can switch features off; `plugin.log` marks them `degraded`.
- Use the `chatBridgeNoEcho` module; the stock `chatBridge` echoes server messages to Discord.
- The console `location` command fails for place names with spaces.
