# Takaro RuneScape: Dragonwilds Connector

The RuneScape: Dragonwilds **Linux dedicated server** loads one native `libtakaro-dragonwilds.so`
connector. It connects directly to Takaro; there is no sidecar. Players install nothing and keep the
vanilla game client. The library must be preloaded on the game binary's launch line.

## Install

### 1. Prepare the server

You need a Dragonwilds **Linux dedicated server** (Steam app 4019830) whose launch command you can
change, with its `RSDragonwildsServer-Linux-Shipping.sym` file next to the server binary (SteamCMD
downloads it; `app_update 4019830 validate` restores it). Set the server's `OwnerId` to the owner's
EOS Player ID (shown at the bottom of the game client's Settings); without it the server never loads a
world and its log says `[OwnerId] ... empty`. Create a Takaro **Generic** game server and copy its
registration token. Client and server must run the same game version.

Look up your server's Steam build: it is the `"buildid"` value in
`steamapps/appmanifest_4019830.acf` inside the server's install directory. Each release ships one
plugin archive per supported build. The latest release supports builds 26161880 (the current public
build), 26087591, 25991736, 25808123, 25630937, 25501739 and 25465077.

### 2. Download and copy

Download the latest release: https://takaro.io/connectors/dragonwilds

From the latest `dragonwilds-vX.Y.Z` release on the releases page

> https://github.com/gettakaro/connectors/releases

download `SHA256SUMS` and the plugin archive for your build,
`takaro-dragonwilds-plugin-linux-<build>-<version>.tar.gz`, for example
`takaro-dragonwilds-plugin-linux-26161880-<version>.tar.gz`. `takaro-dragonwilds-plugin.tar.gz` is a
copy of the archive for the current public build. Do not use the "Source code" archives, the
`dragonwilds-dev` pre-release or a `pr-<number>-dragonwilds` build; those are untested rolling
builds. Check and unpack:

```bash
sha256sum -c SHA256SUMS --ignore-missing
tar -xzf takaro-dragonwilds-plugin-linux-<build>-<version>.tar.gz
(cd TakaroDragonwilds && sha256sum -c SHA256SUMS)
mkdir -p data/dragonwilds-plugin data/dragonwilds-state
cp TakaroDragonwilds/libtakaro-dragonwilds.so data/dragonwilds-plugin/
cp TakaroDragonwilds/env.example .env   # Docker only: the game server settings
```

Keep the library **outside the Steam tree**: SteamCMD `validate` deletes files it does not know. The
archive also holds `takaro-target.json` (the build it was made for), `docker-compose.example.yml`,
`INSTALL.md` (upgrade and rollback), the `scripts/drain-legacy.py` upgrade tool and the licenses.

### 3. Configure

The connector's one config file is `takaro.cfg` next to the library
(`data/dragonwilds-plugin/takaro.cfg`). The connector creates it on first start, with a new
identity and the name `Dragonwilds (<8 characters>)`. Open it, paste the registration token after
`TAKARO_REGISTRATION_TOKEN=` and save. For the URL, token, identity and name, saving is enough,
also while the server runs: the connector connects within a few seconds, no restart. Every other
key in the table below can also go in `takaro.cfg`; those apply after a restart. You can also
create the file before the first start; leave `TAKARO_IDENTITY_TOKEN` empty and the connector
fills in a new identity (keep it once it is set):

```ini
# Takaro connector settings for this RuneScape: Dragonwilds server. One KEY=VALUE per line.
# Saving is enough for the four keys below: the connector re-reads them within a few
# seconds, no restart. Any other key from the README can be added here too; those apply
# after a restart. A non-empty environment variable with the same name wins over the line here.

# The registration token of your Takaro Generic game server.
TAKARO_REGISTRATION_TOKEN=

# This server's identity in Takaro, created by the connector. Keep it.
TAKARO_IDENTITY_TOKEN=

# The name Takaro shows for this server; unique in your Takaro domain.
TAKARO_SERVER_NAME=

TAKARO_WS_URL=wss://connect.takaro.io/
```

An empty `TAKARO_SERVER_NAME` means `Dragonwilds (<first 8 identity characters>)`. Every key can
also be set in the **game process** environment (`.env` with the Compose example); a non-empty
environment variable wins over `takaro.cfg` and needs a restart to change.

| Key | Required | What to put there |
|---|---|---|
| `TAKARO_REGISTRATION_TOKEN` | yes | The Generic game server's registration token. |
| `TAKARO_IDENTITY_TOKEN` | no | Filled in by the connector; keep it. |
| `TAKARO_SERVER_NAME` | no | Server name shown in Takaro; unique in your Takaro domain. |
| `TAKARO_WS_URL` | no | Default `wss://connect.takaro.io/`. |
| `TAKARO_PROTOCOL` | no | Set `0` to stay on Takaro protocol version 0 (read at start). By default the connector offers versions 0 and 1 and Takaro picks. |
| `TAKARO_SENDER_NAME` | no | Sender name for broadcasts; default is the server name. |
| `TAKARO_STATE_DIR` | Docker: yes | A persistent, writable directory, e.g. `/opt/takaro-state`. Default `<server>/RSDragonwilds/Binaries/Linux/takaro`. |
| `DRAGONWILDS_LOG_FILE` | no | The server log; found from the binary by default. |
| `DRAGONWILDS_LOG_TAIL` / `DRAGONWILDS_LOG_EVENTS` / `DRAGONWILDS_LOG_RATE` | no | `auto` / `filtered` / `40` (log lines per 30 s) by default. |
| `TAKARO_PLUGIN_TOKEN` | no | Enables loopback diagnostics on `127.0.0.1:18890`. |
| `TAKARO_CA_FILE` | no | Extra trusted CA file; certificate checks always stay on. |
| `TAKARO_TICK_BUDGET_US` | no | Game-thread time per tick for the connector, default `500`. |
| `TAKARO_SYM_PATH` | no | Only if the `.sym` file is not next to the binary. |

Never commit or share a filled-in `takaro.cfg` or `.env`.

### 4. Load the plugin into the server

Preload the library **on the game binary only**. SteamCMD is 32-bit and fails with a 64-bit preload,
so never set `LD_PRELOAD` for the whole container, user or service:

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so ./RSDragonwildsServer.sh -log
```

With Docker, mount `data/dragonwilds-plugin` (writable, so the connector can create `takaro.cfg`) at
`/opt/takaro` and `data/dragonwilds-state` at `/opt/takaro-state`; the image's entrypoint must apply `LD_PRELOAD="${TAKARO_PLUGIN_SO}"` to the
server launch line only (see `docker-compose.example.yml`). Start only the game service.

### 5. Verify

- `grep libtakaro /proc/<server pid>/maps` lists `libtakaro-dragonwilds.so`.
- The server console shows `[Takaro] connected to Takaro as "..."`, and Takaro shows the game server
  as **online** within a minute of the world loading.

Without a token, without an identity in a read-only plugin folder, or when Takaro refuses the token
or the server name, the console shows a `[Takaro]` banner naming the file to fix (in Docker, `/opt/takaro/takaro.cfg` is
`data/dragonwilds-plugin/takaro.cfg`). Details: `plugin.log` in the plugin data directory
(`<server>/RSDragonwilds/Binaries/Linux/takaro/`).

### 6. Upgrade

**From the 0.2.x sidecar:** the old sidecar and the native connector must never run with the same
Takaro identity at once, so the move needs a short maintenance window. Follow
[INSTALL.md](INSTALL.md) ("Upgrade from the 0.2.x sidecar"; it is also in the archive); it drains
the sidecar with `scripts/drain-legacy.py`, imports its state and explains rollback.

**After a game update** you normally do nothing: the plugin re-reads the new `.sym` file on start.
When a release adds an archive for your new build, switch to that one. To upgrade the connector,
stop the server, replace only `libtakaro-dragonwilds.so`, and start again.
Your `takaro.cfg` next to it, `.env` and the state directory are never touched.

## What works, what doesn't

✅ = works, ⚠️ = works with a caveat or not yet verified, ❌ = unsupported.

Verified live with a real game client on **2026-10-17** against Steam build **25991736**: connection,
player list, messages, console commands, give item, teleport, kick, ban and unban, shutdown, the
join, leave, chat, death and kill events, the teleports module and game chat to Discord. Game
name, version and icon were verified on **2026-10-21** against a Takaro that speaks protocol 1.
Every other ✅ row was verified live on **2026-10-02** against build **25501739**, the first build of
the native connector.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | Plugin connects to Takaro itself; no sidecar. |
| Player list | ✅ | Character name and EOS id. |
| Single player lookup | ⚠️ | Works online; Takaro never asks about offline players. |
| Player location | ✅ |  |
| Player inventory | ✅ |  |
| Give an item | ✅ | Appears in the inventory without a relog. |
| Item catalogue | ✅ | 1536 items with in-game names. |
| Entity catalogue | ⚠️ | Only creatures the server has loaded so far. |
| Locations / points of interest | ⚠️ | Lodestones only; Takaro does not use this list. |
| Run a console command | ✅ | Connector command set; unknown commands are refused. |
| Broadcast a message | ✅ | Shows under the receiving player's name. |
| Whisper a player | ✅ |  |
| Teleport a player | ✅ | Module teleports included. |
| Kick a player | ✅ |  |
| Ban a player (timed and permanent) | ✅ | Banned players are dropped about a second after joining. |
| Unban a player | ✅ | Works immediately, also after a restart. |
| Ban list | ✅ | Includes the expiry of timed bans. |
| Shut the server down | ✅ | Saves first, then stops. |
| Player joined event | ✅ |  |
| Player left event | ✅ |  |
| Player chat event | ✅ |  |
| Player death event | ✅ | Names the creature that killed the player. |
| Entity kill event | ✅ | Creature name and held weapon. |
| Log events | ✅ | Passwords removed; rate-limited to stay under Takaro's limit. |
| World map and player positions | ⚠️ | Needs Takaro protocol 1 (not yet on takaro.io). The connector gives Takaro the world's map frame and puts players on it. The server has no map picture, so you upload one once in the dashboard (see DEVELOPMENT.md). |
| Game name, version and icon | ⚠️ | Needs Takaro protocol 1 (not yet on takaro.io). Takaro shows the server as RuneScape: Dragonwilds with the running game version (for example `1.0.1.1`), the game icon and the connector version. |
| Modules: chat commands | ✅ |  |
| Modules: hooks | ✅ |  |
| Modules: cronjobs | ✅ |  |
| Modules: teleports (`@settp`, `@tp`, …) | ✅ |  |
| Modules: server messages / onboarding | ✅ |  |
| Shop: buy in game | ✅ |  |
| Shop: order in Takaro and claim in game | ✅ |  |
| Shop: bundle of several items | ✅ |  |
| Shop: order while offline, claim later | ✅ |  |
| Shop: not enough currency | ✅ | Refused, nothing deducted. |
| Economy: currency | ✅ |  |
| Economy: balance in game | ✅ |  |
| Discord: game chat → Discord | ✅ | Use the `chatBridgeNoEcho` module. |
| Discord: Discord → game chat | ⚠️ | Not yet verified with the native connector. |
| Discord: module hook / cronjob posts | ✅ |  |
| Discord: join/leave notices | ✅ |  |
| Discord: no echo of server messages | ✅ | Use the `chatBridgeNoEcho` module. |
| Events while the Takaro connection is down | ✅ | Queued on disk and delivered after reconnect. |
| Reconnects after a server or container restart | ✅ |  |
| No duplicate events after a connector restart | ✅ |  |
| Survives a network drop to Takaro | ✅ |  |
| Timed bans expire on their own | ✅ | Lifted by the connector, also across restarts. |
| Keeps running after a game update breaks a feature | ✅ | That feature reports degraded; the rest keeps working. |

### Known issues

- The server idles until `OwnerId` is set to the owner's EOS Player ID.
- Client and server are version-locked: update both together.
- Upgrading from the 0.2.x sidecar needs a short maintenance window.
- Broadcasts and whispers show under the receiving player's name, with a prefix.
- A banned player connects briefly before the plugin drops them again.
- Whispers are delivered; hiding them from other players is not yet verified.
- The entity catalogue only lists creatures the server has loaded so far.
- The server log holds the world password; the connector redacts it from Takaro.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
