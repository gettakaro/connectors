# Takaro Conan Exiles Connector

A Node.js bridge (version **1.0.0**) that runs next to a Conan Exiles dedicated server and connects
it to Takaro over RCON and the server's log files. Validated against a **Conan Exiles Enhanced**
Linux dedicated server (SteamCMD app `443030`). Players do not install anything.

## Install

### 1. Before you start

You need:

- A **Conan Exiles dedicated server** (Linux or Windows) that you can stop, start and copy files to.
- **Shell access on the server host**, with **Node.js 22** installed. The bridge is a separate
  process that runs on the same machine as the game server.
- **RCON enabled** on the Conan server, and its password. Either add it to `Game.ini`:

  ```ini
  [RconPlugin]
  RconEnabled=1
  RconPassword=YourRconPassword
  RconPort=25575
  ```

  or pass the equivalent launch flags
  (`-RconEnabled=1 -RconPassword=YourRconPassword -RconPort=25575`). Restart Conan afterwards.
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).
- **For in-game chat only:** the **Enhanced Pippi** mod on the server (workshop ID `3725018456`).
  Without a chat mod the bridge cannot write normal chat lines — see step 4 and the table below.

> **About the Takaro Conan mod.** This repo contains a specification for a Takaro-owned
> `TakaroConan.pak` under `mod/TakaroConanBridge/`, but **no `.pak` is built or shipped**. Building
> it requires the Conan Exiles DevKit (Windows + Unreal cook toolchain), so there is nothing to
> download. Everything below works without it; chat delivery uses Enhanced Pippi instead.

### 2. Download the bridge

Download **`takaro-conan-exiles-bridge.zip`** from the `conan-exiles-v1.0.0` release on the releases
page:

> https://github.com/gettakaro/connectors/releases

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/conan-exiles-v<version>/takaro-conan-exiles-bridge.zip`

Use `conan-exiles-v1.0.0` or newer. Do not use the `conan-exiles-dev` pre-release; that is an
untested rolling build.

The zip contains a single folder, `TakaroConanExiles/`. That whole folder is the bridge.

### 3. Copy it into place

Unzip it anywhere on the server host that the Conan server user can read and write — it does **not**
go inside the Conan game folder:

```
<anywhere>/TakaroConanExiles/
    dist/                      # the compiled bridge
    scripts/
    package.json
    package-lock.json
    TakaroConfig.example.txt
    README.md
    README.release.txt
```

Then install the runtime dependencies in that folder:

```bash
cd TakaroConanExiles
npm ci --omit=dev
```

Examples:

- Linux: `/home/steam/TakaroConanExiles/`
- Windows: `C:\TakaroConanExiles\`

### 4. Configure

Copy the example config and edit it:

```bash
cp TakaroConfig.example.txt TakaroConfig.txt
```

Fill in these keys in `TakaroConfig.txt`:

```text
registrationToken=your-registration-token-here
serverName=Conan Exiles Server
takaroWsUrl=wss://connect.takaro.io/

rconHost=127.0.0.1
rconPort=25575
rconPassword=your-rcon-password
rconCommandGapMs=1000

httpPort=3010
pollIntervalMs=10000
enableLogEvents=true
logFiles=
databasePath=
itemCatalogPath=
```

- `registrationToken`, `rconHost`, `rconPort`, `rconPassword` are the ones you must set. Leave
  `identityToken` empty — the bridge fills it in itself.
- `logFiles` — comma-separated absolute paths to Conan's logs; needed for chat, death and log
  events. Typical:
  `<server>/ConanSandbox/Saved/Logs/ConanSandbox.log`, plus `ConanSandbox_2.log` and
  `RconCommandLog.log`.
- `databasePath` — absolute path to Conan's save database `game_0.db`. Without it, player location,
  inventory and the item/entity/location lists come back empty (see the table below).
- `pollIntervalMs` — leave at `10000` or higher. Conan's RCON karma protection blocks a bridge that
  polls faster.
- Leave `requireModSourceAttribution=false`; it is only for validating an unreleased Takaro `.pak`.

**For in-game chat**, start the chat helper as a second process, pointed at Enhanced Pippi:

```bash
BRIDGE_CONFIG=/path/to/TakaroConfig.txt \
TAKARO_CONAN_CHAT_MOD=pippi \
npm run mod-helper
```

Note: the released zip ships only the compiled bridge, so `npm run mod-helper` needs the source
checkout of `games/conan-exiles/bridge` and a full `npm ci` (not `--omit=dev`) on the server host.
Without this helper, Takaro messages fail with a clear error instead of appearing in chat.

Then start the bridge:

```bash
npm start
```

### 5. Check that it worked

Ask the bridge's own health endpoint on the server host:

```bash
curl http://127.0.0.1:3010/health
```

It reports the connection state and the `gameServerId` Takaro assigned. If the registration token
was rejected, `/health` shows `takaroIdentifyError` and the bridge stops retrying until you fix the
token.

And in Takaro, the game server shows as **online** and lists your online players. If it stays
offline, check, in order: the `registrationToken` in `TakaroConfig.txt`, that RCON accepts your
password, and that the bridge process is still running.

### 6. Upgrading

Stop the bridge (and the chat helper). Unzip the new version over the old folder, or into a new
folder and copy your `TakaroConfig.txt` across — the config is not part of the zip, so it survives.
Run `npm ci --omit=dev` again, then start the bridge. The Conan server itself does not need to
restart.

## What works, what doesn't

Status below comes from the recorded capability data and the live checks run on **2026-06-20** and
**2026-06-21** against a real Conan Exiles Enhanced dedicated server with Enhanced Pippi and one
real player connected. Anything that was never exercised in a live test says so.
✅ = works, ⚠️ = works with a caveat or is unproven, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The bridge registers with Takaro and reachability checks succeed against the live server. |
| Server restart / reconnect | ⚠️ | The bridge is built to reconnect, but recovery after a Conan restart or a network outage was not verified in a live test. |
| Player list | ✅ | Names and Steam64 ids, read from Conan's `listplayers`. This is how Takaro loads players for Conan. |
| Single player lookup | ✅ | Looking up one player by their game id returns the same data as the player list. |
| Player location | ⚠️ | Only with `databasePath` set: coordinates are read from Conan's save database. Without it, Takaro gets `0,0,0`. |
| Player inventory | ⚠️ | Only with `databasePath` set: read from the save database. Without it, the inventory comes back empty. |
| Item catalogue | ⚠️ | Not a real catalogue — it lists only the item ids that already exist in the save database, and only with `databasePath` set. |
| Entity catalogue | ⚠️ | Same: only the creature/actor classes already present in the save database, and only with `databasePath` set. |
| Locations / points of interest | ⚠️ | Returns saved player character positions, not real Conan points of interest, and only with `databasePath` set. |
| Chat messages from players | ⚠️ | Live player chat reached Takaro with the correct player attached, but only via Enhanced Pippi's log lines. Without Pippi, chat parsing is best effort and may pick up nothing. |
| Broadcast a message | ⚠️ | Confirmed visible in game, but only through Enhanced Pippi's `server` command with the chat helper running. Vanilla Conan has no way to write a normal chat line. |
| Whisper a player | ⚠️ | Pippi accepted the direct message and reported it sent; it was not confirmed on a client, and it needs the player's Conan **character** name to resolve. |
| Give an item | ⚠️ | Spawns the item through Conan's admin relay and the server reports success; the player must be **online**, and the item actually landing in their inventory was not confirmed in game. |
| Teleport a player | ⚠️ | Triggers Conan's teleport streaming for an **online** player; the move was not confirmed on a client. |
| Run a console command | ✅ | Commands are sent over RCON and the raw output comes back to Takaro. |
| Kick a player | ⚠️ | The command exists on the server and the bridge sends it, but no live kick was performed. |
| Ban a player (timed and permanent) | ⚠️ | Same: the ban command is wired up but was never executed against a live player. |
| Unban a player | ⚠️ | Same: wired up, never executed live. |
| Ban list | ⚠️ | Reads Conan's `listbans`. Verified against an empty list; output from a server with many bans is the weaker case and Conan's format varies by version. |
| Shut the server down | ⚠️ | The bridge sends Conan's shutdown command, but no live shutdown was performed. |
| Player joined event | ⚠️ | Derived from changes in the player list, so it can lag by up to one poll (10 s by default). Not confirmed arriving in Takaro in a live test. |
| Player left event | ⚠️ | Same as joins: derived from the player list, not confirmed live. |
| Player chat event | ⚠️ | See "Chat messages from players" — Enhanced Pippi only. |
| Player death event | ⚠️ | Best effort, parsed out of Conan's log lines. No live death was captured in a test. |
| Entity kill event | ⚠️ | Best effort from the same log lines, with the killer resolved only if they are online. No live kill was captured. |
| Log events | ⚠️ | Log tailing works against real Conan logs, but Takaro does not store server log lines as searchable events. |
| Map info | ⚠️ | The bridge answers with an empty/disabled map; Conan exposes no map metadata. |
| Map tiles | ❌ | The Takaro API does not support map tiles for Generic-connector servers. Nothing on the game server side changes that. |
| Discord chat bridge | ⚠️ | Never tested for Conan. Game → Discord depends on Pippi chat parsing; Discord → game depends on the Pippi chat helper. |
| Shop & economy | ⚠️ | Never tested for Conan. Item delivery would go through the same online-player-only spawn route as "Give an item". |

### Known issues

- **Chat needs Enhanced Pippi.** Conan has no vanilla command that writes a normal chat line —
  `broadcast` shows a screen overlay instead. Without Enhanced Pippi and the chat helper, Takaro
  messages fail rather than appear. Use the Enhanced workshop item `3725018456`; the legacy Pippi
  `880454836` loads but registers no commands.
- **Half the read features need the save database.** Location, inventory, and the item, entity and
  location lists are empty unless `databasePath` points at Conan's `game_0.db` on the same host.
- **Give item and teleport only work on online players.** They go through Conan's admin relay to a
  connected client; offline players cannot be targeted.
- **Conan's RCON throttles you.** Polling faster than the default 10 s, or running several tools
  against the same server at once, trips Conan's RCON karma and requests start being denied.
- **No map.** Takaro's API does not support map tiles for Generic-connector servers.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
