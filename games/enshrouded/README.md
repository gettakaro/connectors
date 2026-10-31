# Takaro Enshrouded Connector

One file, `dbghelp.dll`, that connects an Enshrouded dedicated server (Windows, or Linux under
Proton) to Takaro. Players do not install anything.

## Install

You need: an Enshrouded dedicated server on build **1273043** (Enshrouded 1.0) or **1267041**, and a
Takaro game server of type **Generic** with its registration token. With Docker, use
`mornedhels/enshrouded-server:dev-proton`.

### 1. Download

From https://takaro.io/connectors/enshrouded, download
`takaro-enshrouded-plugin-proton-1273043-<version>.zip`. It works on every supported build, on
Windows and under Proton. Do not use the `enshrouded-dev` or `pr-*` pre-releases.

### 2. Install

Stop the server and copy `dbghelp.dll` from the zip next to `enshrouded_server.exe`:

```text
<server folder>/enshrouded_server.exe
<server folder>/dbghelp.dll
```

Under Wine/Proton, set `WINEDLLOVERRIDES=dbghelp=n,b` on the game server. A Windows server needs
nothing extra.

**With Docker**, use [`docker-compose.example.yml`](docker-compose.example.yml),
[`.env.example`](.env.example) and [`server/enshrouded-updater`](server/enshrouded-updater) from
this folder. Copy your server files into `data/enshrouded/server` first, then:

```bash
mkdir -p data/enshrouded/server/takaro/plugin
cp TakaroEnshrouded/dbghelp.dll data/enshrouded/server/takaro/plugin/
cp .env.example .env    # fill in TAKARO_REGISTRATION_TOKEN and the role passwords
docker compose -f docker-compose.example.yml --env-file .env up -d
```

Turn off automatic game updates on your host, so the game does not move to a build the connector
does not support yet.

### 3. Configure

On start the connector creates `takaro/plugin.json` next to `enshrouded_server.exe`. Paste your
token and save; it connects within a few seconds, no restart needed:

```json
{
  "registrationToken": "<your registration token>",
  "identityToken": "",
  "name": "",
  "url": "wss://connect.takaro.io/"
}
```

Leave `identityToken` empty; the connector fills it in. Keep it once set.

| Optional setting | What it does |
|---|---|
| `name` | Server name in Takaro. |
| `serverChatName` | Sender name on messages from Takaro, used when none is set for the server in Takaro. |
| Map image | Upload one on the server's map page in Takaro; see [INSTALL.md](INSTALL.md), "Map". |

### 4. Check that it works

Within about 20 seconds the console and `takaro/plugin.log` show
`[Takaro] connected to Takaro as "<name>"` and the server shows as online in Takaro. Otherwise a
block of `*` lines says what to fix (missing token, wrong token or a name already in use).

**Upgrading:** stop the server and replace only `dbghelp.dll` (with Docker:
`data/enshrouded/server/takaro/plugin/dbghelp.dll`). From 0.5.0 (plugin + sidecar), follow
[INSTALL.md](INSTALL.md).

## What works, what doesn't

Checked on game build 1273043 (Enshrouded 1.0), 2026-10-21. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | |
| Player list | ✅ | Character names, SteamID64. |
| Player location | ✅ | |
| Teleport | ✅ | To the exact coordinates; pick a spot above ground. |
| Player inventory | ✅ | |
| Item list | ✅ | |
| Entity list | ⚠️ | Some creatures get a name built from the internal code. |
| Locations | ✅ | About 800 named map places. |
| World map | ⚠️ | You upload the map image yourself. Not checked yet. |
| Send a message | ✅ | Shown as a notice with the sender name in front. |
| Console commands | ✅ | The connector's own set (`help` lists it); the game has no console. |
| Kick | ✅ | |
| Ban and unban | ✅ | Ban needs the player online. |
| Ban list | ✅ | |
| Give an item | ✅ | |
| Shop | ✅ | |
| Shut down the server | ✅ | Saves, then quits. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | With the creature's name. |
| Entity killed | ⚠️ | Magic kills not checked. |
| Server log lines | ⚠️ | Takaro does not keep them. |
| Discord chat bridge | ⚠️ | Discord to game works; game to Discord not confirmed. Use `chatBridgeNoEcho`. |
| Modules | ✅ | |
| Game name, version and icon | ✅ | |
| Restarts and outages | ✅ | Back within about 25 seconds; events are kept until Takaro is reachable. |
| Game updates | ✅ | A feature a new build breaks reports degraded; the rest keeps working. |

## Known issues

- Only builds 1273043 and 1267041 are supported. On other builds the item and creature lists and
  giving items switch off.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
