# Takaro RuneScape: Dragonwilds Connector

A native library that the RuneScape: Dragonwilds **Linux** dedicated server preloads. Players do
not install anything.

## Install

You need: a Dragonwilds **Linux** dedicated server whose launch command you can change, with
`RSDragonwildsServer-Linux-Shipping.sym` next to the server binary (SteamCMD downloads it), its
`OwnerId` set to the owner's EOS Player ID (bottom of the game client's Settings), and a Takaro game
server of type **Generic** with its registration token.

### 1. Download

From https://takaro.io/connectors/dragonwilds, download
`takaro-dragonwilds-plugin-linux-<build>-<version>.tar.gz` for your server build. Your build is the
`"buildid"` in `steamapps/appmanifest_4019830.acf`. Do not use the `dragonwilds-dev` or `pr-*`
pre-releases.

### 2. Install

Unpack and copy the library to a folder **outside the Steam tree** (SteamCMD `validate` deletes
unknown files):

```bash
tar -xzf takaro-dragonwilds-plugin-linux-<build>-<version>.tar.gz
mkdir -p data/dragonwilds-plugin data/dragonwilds-state
cp TakaroDragonwilds/libtakaro-dragonwilds.so data/dragonwilds-plugin/
```

Preload it **on the game server command only**, never for the whole container, user or service
(SteamCMD breaks):

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so ./RSDragonwildsServer.sh -log
```

With Docker, mount `data/dragonwilds-plugin` (writable) at `/opt/takaro` and
`data/dragonwilds-state` at `/opt/takaro-state`, and set `TAKARO_STATE_DIR=/opt/takaro-state`.
`docker-compose.example.yml` in the archive shows the setup.

### 3. Configure

On first start the connector creates `takaro.cfg` next to the library. Paste your token and save;
it connects within a few seconds, no restart needed:

```ini
TAKARO_REGISTRATION_TOKEN=your-registration-token-here
TAKARO_IDENTITY_TOKEN=
TAKARO_SERVER_NAME=
TAKARO_WS_URL=wss://connect.takaro.io/
```

Leave `TAKARO_IDENTITY_TOKEN` empty; the connector fills it in. Keep it once set.

| Optional setting | What it does |
|---|---|
| `TAKARO_SERVER_NAME` | Server name in Takaro. |
| `TAKARO_SENDER_NAME` | Sender name on messages from Takaro, used when none is set for the server in Takaro (restart to change). |

The same keys can be set as environment variables instead.

### 4. Check that it works

The console shows `[Takaro] connected to Takaro as "..."` and the server shows as online in Takaro
within a minute of the world loading. Otherwise the console shows a `[Takaro]` banner naming the
file to fix.

**Upgrading:** stop the server and replace only `libtakaro-dragonwilds.so`. From the 0.2.x sidecar,
follow [INSTALL.md](INSTALL.md).

## What works, what doesn't

Checked on server build 25991736, 2026-10-17. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects; events from an outage are delivered afterwards. |
| Player list | ✅ | Character name and EOS id. |
| Player location | ✅ | |
| Teleport | ✅ | |
| Player inventory | ✅ | |
| Item list | ✅ | |
| Entity list | ⚠️ | Only creatures the server has loaded so far. |
| Locations | ⚠️ | Lodestones only. |
| World map | ⚠️ | After a map upload in Takaro. Not checked yet. |
| Send a message | ✅ | Shown as `[<chat name>] text`. |
| Console commands | ✅ | The connector's own command set. |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent. |
| Ban list | ✅ | |
| Give an item | ✅ | |
| Shop | ✅ | |
| Shut down the server | ✅ | Saves first. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | Names the creature. |
| Entity killed | ✅ | |
| Server log lines | ✅ | The world password is removed. |
| Discord chat bridge | ⚠️ | Game to Discord works (use `chatBridgeNoEcho`); Discord to game not checked. |
| Modules | ✅ | |
| Game name, version and icon | ⚠️ | Not checked yet. |
| Game updates | ✅ | A feature the update breaks reports degraded; the rest keeps working. |

## Known issues

- The server idles until `OwnerId` is set.
- Client and server must be on the same game version.
- A banned player connects briefly before being dropped.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
