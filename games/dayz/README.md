# Takaro DayZ Connector

A server-side DayZ mod plus a small sidecar program that connect a DayZ dedicated server to
Takaro. DayZ's server scripts cannot open the WebSocket Takaro uses, so the sidecar runs next to
the server and holds that connection. Players do not install anything.

Written by [mad-001](https://github.com/mad-001) as
[DayZ-Takaro-Integration](https://github.com/mad-001/DayZ-Takaro-Integration) (MIT).

## Install

You need: a DayZ dedicated server whose start line you can change, Node.js 22
or Docker on the same machine, and a Takaro game server of type **Generic** with its
registration token.

### 1. Download

From https://takaro.io/connectors/dayz, download both files for your server build:

- `takaro-dayz-mod-<target>-<version>.zip` (the server mod)
- `takaro-dayz-sidecar-<target>-<version>.tar.gz` (the sidecar)

### 2. Install

**Mod.** Unzip the mod into the server folder and add it to the start line as a server mod
(never `-mod=`, or clients are asked to download it):

```text
<server>/@TakaroIntegration/Addons/TakaroIntegration.pbo
DayZServer -config=serverDZ.cfg ... -serverMod=@TakaroIntegration
```

**Sidecar.** Unpack the archive on the same machine. With Docker, rename its folder to `sidecar`
and move `docker-compose.example.yml` next to it. Without Docker, run `npm ci --omit=dev` inside
it. `README.txt` in the archive has both ways.

### 3. Configure

Give the sidecar your registration token and a server name, in a `.env` file next to the compose
file (Docker) or as environment variables (Node.js):

```text
TAKARO_REGISTRATION_TOKEN=<token from the Takaro dashboard>
TAKARO_SERVER_NAME=My DayZ Server
```

Start it with `docker compose -f docker-compose.example.yml up -d --build` or
`node dist/index.js`, then restart the DayZ server. The mod needs no settings: it finds the
sidecar on `127.0.0.1:8088`.

| Setting | Where | Default |
|---|---|---|
| Sidecar port | `BRIDGE_LOCAL_PORT` (sidecar) and `TakaroApiUrl` in `profiles/TakaroIntegration/config.json` (mod) | `8088` |
| Server log lines in Takaro | `DAYZ_LOG_DIR` (sidecar): the server's profiles folder | off |
| World map | `DAYZ_LOG_DIR` as above; the map is drawn after the first start | off |
| Server chat name | Takaro's server chat name setting | `Server` |

### 4. Check that it works

The sidecar prints `Identified as gameServerId=…` and the server shows as online in Takaro. The
server's script log shows `[Takaro] Bridge v<version> starting`. With a wrong token the sidecar
prints `Identification failed` and the server stays offline.

**Upgrading:** replace both downloads together and restart the sidecar and the DayZ server.

## What works, what doesn't

Checked on DayZ 1.30 (Linux server), 2026-11-27. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | |
| Player list | ✅ | Steam ID for every player; DayZ does not expose player IPs |
| Player location | ✅ | |
| Teleport | ✅ | |
| Player inventory | ✅ | |
| Item list | ✅ | Display names; colour and faction variants named apart |
| Entity list | ✅ | Infected and animals |
| Locations | ✅ | Chernarus towns by default; edit `profiles/TakaroIntegration/locations.json` |
| World map | ✅ | Drawn from the server's own world |
| Send a message | ✅ | To everyone or to one player |
| Console commands | ⚠️ | A fixed set of commands; type `help` for the list |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed bans end on their own |
| Ban list | ✅ | |
| Give an item | ✅ | |
| Shop | ✅ | |
| Shut down the server | ✅ | |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | With the killer and weapon when there is one |
| Entity killed | ✅ | Infected and animals, with the weapon |
| Server log lines | ✅ | With `DAYZ_LOG_DIR` set |
| Discord chat bridge | ✅ | |
| Modules | ✅ | |
| Game name, version and icon | ✅ | |
| Characters | ✅ | Each life is a character; death ends it |
| Restarts and outages | ✅ | Events wait while Takaro is unreachable |

## Known issues

- DayZ has no levels or skills, so there is no player progression.
- Checked on a Linux server. A Windows server uses the same two files but has not been checked yet.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
