# Takaro RuneScape: Dragonwilds Connector

A native library, `libtakaro-dragonwilds.so`, that the RuneScape: Dragonwilds **Linux** dedicated
server preloads. It connects to Takaro by itself, with no sidecar. Players do not install anything
and keep the normal game client.

## Install

### 1. Before you start

- A Dragonwilds **Linux** dedicated server (Steam app 4019830) whose launch command you can change,
  with its `RSDragonwildsServer-Linux-Shipping.sym` file next to the server binary (SteamCMD
  downloads it; `app_update 4019830 validate` restores it).
- The server's `OwnerId` set to the owner's EOS Player ID (shown at the bottom of the game client's
  Settings). Without it the server never loads a world and its log says `[OwnerId] ... empty`.
- Client and server on the same game version.
- A Takaro game server of type **Generic** and its **registration token**.

### 2. Download

Download the latest release from https://takaro.io/connectors/dragonwilds (or from the latest
`dragonwilds-vX.Y.Z` release at https://github.com/gettakaro/connectors/releases). Do not use the
`dragonwilds-dev` pre-release or a `pr-<number>-dragonwilds` build.

There is one archive per server build: `takaro-dragonwilds-plugin-linux-<build>-<version>.tar.gz`.
Your build is the `"buildid"` value in `steamapps/appmanifest_4019830.acf` in the server's install
folder. The latest release supports builds 26161880 (the current public build), 26087591,
25991736, 25808123, 25630937, 25501739 and 25465077. `takaro-dragonwilds-plugin.tar.gz` is a copy
of the archive for the current public build. Download `SHA256SUMS` too, then check and unpack:

```bash
sha256sum -c SHA256SUMS --ignore-missing
tar -xzf takaro-dragonwilds-plugin-linux-<build>-<version>.tar.gz
(cd TakaroDragonwilds && sha256sum -c SHA256SUMS)
```

### 3. Install

Copy the library to a folder **outside the Steam tree** (SteamCMD `validate` deletes files it does
not know):

```bash
mkdir -p data/dragonwilds-plugin data/dragonwilds-state
cp TakaroDragonwilds/libtakaro-dragonwilds.so data/dragonwilds-plugin/
cp TakaroDragonwilds/env.example .env   # Docker only: the game server settings
```

```
data/
    dragonwilds-plugin/
        libtakaro-dragonwilds.so
        takaro.cfg              (created by the connector on first start)
    dragonwilds-state/
```

Preload the library **on the game binary only**. SteamCMD is 32-bit and fails with a 64-bit
preload, so never set `LD_PRELOAD` for the whole container, user or service:

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so ./RSDragonwildsServer.sh -log
```

With Docker, mount `data/dragonwilds-plugin` (writable) at `/opt/takaro` and
`data/dragonwilds-state` at `/opt/takaro-state`. The image's entrypoint must apply
`LD_PRELOAD="${TAKARO_PLUGIN_SO}"` to the server launch line only (see
`docker-compose.example.yml` in the archive). Start only the game service.

The archive also holds `takaro-target.json` (the build it was made for), `INSTALL.md`, the
`scripts/drain-legacy.py` upgrade tool and the licenses.

### 4. Configure

On first start the connector creates `takaro.cfg` next to the library
(`data/dragonwilds-plugin/takaro.cfg`), with a new identity. Paste the registration token after
`TAKARO_REGISTRATION_TOKEN=` and save. For the token, identity, name and URL, saving is enough,
also while the server runs: the connector connects within a few seconds. Other keys need a restart.
You can also create the file before the first start; leave `TAKARO_IDENTITY_TOKEN` empty and the
connector fills it in (keep it once it is set):

```ini
# The registration token of your Takaro Generic game server.
TAKARO_REGISTRATION_TOKEN=

# This server's identity in Takaro, created by the connector. Keep it.
TAKARO_IDENTITY_TOKEN=

# The name Takaro shows for this server; unique in your Takaro domain.
TAKARO_SERVER_NAME=

TAKARO_WS_URL=wss://connect.takaro.io/
```

Every key can also be set in the game process environment (`.env` with the Compose example); a
non-empty environment variable wins over `takaro.cfg` and needs a restart to change.

| Key | Required | What to put there |
|---|---|---|
| `TAKARO_REGISTRATION_TOKEN` | yes | The Generic game server's registration token. |
| `TAKARO_IDENTITY_TOKEN` | no | Filled in by the connector; keep it. |
| `TAKARO_SERVER_NAME` | no | Name shown in Takaro; unique in your domain. Empty means `Dragonwilds (<first 8 identity characters>)`. |
| `TAKARO_WS_URL` | no | Default `wss://connect.takaro.io/`. |
| `TAKARO_PROTOCOL` | no | Set `0` only to turn off the newer Takaro features (map, game info); read at start. |
| `TAKARO_SENDER_NAME` | no | Sender name on messages from Takaro (alias `TAKARO_SERVER_CHAT_NAME`); see below. |
| `TAKARO_STATE_DIR` | Docker: yes | A persistent, writable folder, e.g. `/opt/takaro-state`. Default `<server>/RSDragonwilds/Binaries/Linux/takaro`. |
| `DRAGONWILDS_LOG_FILE` | no | The server log; found from the binary by default. |
| `DRAGONWILDS_LOG_TAIL` / `DRAGONWILDS_LOG_EVENTS` / `DRAGONWILDS_LOG_RATE` | no | `auto` / `filtered` / `40` (log lines per 30 s) by default. |
| `TAKARO_PLUGIN_TOKEN` | no | Enables loopback diagnostics on `127.0.0.1:18890`. |
| `TAKARO_CA_FILE` | no | Extra trusted CA file; certificate checks always stay on. |
| `TAKARO_TICK_BUDGET_US` | no | Game-thread time per tick for the connector, default `500`. |
| `TAKARO_SYM_PATH` | no | Only if the `.sym` file is not next to the binary. |

Never share a filled-in `takaro.cfg` or `.env`.

**Server chat name (optional).** Messages from Takaro start with the sender in brackets: `[Takaro] Welcome back`. The game labels every server message with the receiving player's own name, so the bracketed name is the real sender. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `TAKARO_SENDER_NAME` instead (restart to change it); the name set in Takaro wins over it. With neither, the sender is the server name.

### 5. Check that it works

- `grep libtakaro /proc/<server pid>/maps` lists `libtakaro-dragonwilds.so`.
- The server console shows `[Takaro] connected to Takaro as "..."`, and Takaro shows the game
  server as **online** within a minute of the world loading.
- Without a token, with a wrong token or a name Takaro refuses, or without an identity in a
  read-only plugin folder, the console shows a `[Takaro]` banner naming the file to fix (in Docker,
  `/opt/takaro/takaro.cfg` is `data/dragonwilds-plugin/takaro.cfg`). Details are in `plugin.log`
  in `<server>/RSDragonwilds/Binaries/Linux/takaro/`.

### 6. Upgrade

Stop the server, replace only `libtakaro-dragonwilds.so` and start again. `takaro.cfg`, `.env`
and the state folder are never touched. After a game update you normally do nothing: the plugin
re-reads the new `.sym` file on start; when a release adds an archive for your new build, switch
to it. Upgrading from the 0.2.x sidecar needs a short maintenance window: follow
[INSTALL.md](INSTALL.md) ("Upgrade from the 0.2.x sidecar").

## What works, what doesn't

Checked on server build 25991736, 2026-10-17. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects after server or container restarts and network drops; events from an outage are queued on disk and delivered without duplicates. |
| Player list | ✅ | Character name and EOS id. |
| Player location | ✅ | |
| Teleport | ✅ | Module teleports included. |
| Player inventory | ✅ | |
| Item list | ✅ | In-game names. |
| Entity list | ⚠️ | Only creatures the server has loaded so far. |
| Locations | ⚠️ | Lodestones only; Takaro does not use this list. |
| World map | ⚠️ | Players on the world's map frame; upload a map picture once in the dashboard (see [DEVELOPMENT.md](DEVELOPMENT.md)). Not checked on takaro.io yet. |
| Send a message | ⚠️ | Broadcasts and whispers show under the receiving player's name, as `[<chat name>] text`. Whether others can see a whisper is not checked. |
| Console commands | ✅ | The connector's own command set; unknown commands are refused. |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent. A banned player is dropped about a second after joining; timed bans expire on their own, also across restarts. |
| Ban list | ✅ | With the expiry of timed bans. |
| Give an item | ✅ | Appears in the inventory without a relog. |
| Shop | ✅ | Buying in game, orders from Takaro, bundles, offline orders, refusal without enough currency and the in-game balance. |
| Shut down the server | ✅ | Saves first, then stops. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | Names the creature that killed the player. |
| Entity killed | ✅ | Creature name and held weapon. |
| Server log lines | ✅ | The world password is removed; rate-limited to stay under Takaro's limit. |
| Discord chat bridge | ⚠️ | Game chat, module posts and join/leave notices reach Discord (use the `chatBridgeNoEcho` module). Discord to game chat is not checked. |
| Modules | ✅ | Chat commands, hooks, cronjobs and teleports (`@settp`, `@tp`). |
| Game name, version and icon | ⚠️ | Shows RuneScape: Dragonwilds, the game version (for example `1.0.1.1`), the game icon and the connector version. Not checked on takaro.io yet. |
| Game updates | ✅ | A feature the update breaks reports degraded; the rest keeps working. |

## Known issues

- The server idles until `OwnerId` is set to the owner's EOS Player ID.
- Client and server are version-locked: update both together.
- A banned player connects briefly before the connector drops them again.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
