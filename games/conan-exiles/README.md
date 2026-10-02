# Takaro Conan Exiles Connector

A Node.js bridge that runs next to a Conan Exiles dedicated server and connects it to Takaro over
RCON and the server's log files, plus a small native library the Linux server preloads for in-game
chat. Players do not install anything, and neither the server nor the clients need a mod.

It is built against one exact server build: **Conan Exiles Dedicated Server build 25639945**
(Steam app `443030`, branch `public`, the native Linux Enhanced server). The bridge may run beside
another build, but nothing here says it was proven there.

## Install

Download the latest release: https://takaro.io/connectors/conan-exiles

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
- **For in-game chat only:** the **Linux** dedicated server, because chat comes from
  `native/libtakaro-conan-native.so` in the zip, which the server loads with `LD_PRELOAD` (step 4).
  It works only on server build 25639945 and does nothing on any other build. A Windows server
  gets everything except Takaro → game chat.

> **No mods.** Build 25639945 only loads mods built with Dev Kit 1002, so Enhanced Pippi and the
> older `TakaroConan.pak` design under `mod/TakaroConanBridge/` both stop the server from starting
> (`Mod is too old`). Install neither. The native library replaces them.

### 2. Download the bridge

Download **`takaro-conan-exiles-bridge-linux-25639945-<version>.zip`** from the latest
`conan-exiles-vX.Y.Z` release on the releases page:

> https://github.com/gettakaro/connectors/releases

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/conan-exiles-v<version>/takaro-conan-exiles-bridge-linux-25639945-<version>.zip`

`takaro-conan-exiles-bridge.zip` is still published next to it and is the same bytes, so an old
bookmark keeps working. The name in the middle is the server build the bridge was built against.

Use `conan-exiles-v1.0.2` or newer. Do not use the `conan-exiles-dev` pre-release; that is an
untested rolling build.

The zip contains a single folder, `TakaroConanExiles/`. That whole folder is the bridge.

### 3. Copy it into place

Unzip it anywhere on the server host that the Conan server user can read and write — it does **not**
go inside the Conan game folder:

```
<anywhere>/TakaroConanExiles/
    dist/                      # the compiled bridge
    native/                    # libtakaro-conan-native.so: in-game chat (Linux server, step 4)
    scripts/
    package.json
    package-lock.json
    takaro-target.json         # which server build this package was built for
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

**For in-game chat**, start the Conan server with the native library preloaded:

```bash
LD_PRELOAD=/path/to/TakaroConanExiles/native/libtakaro-conan-native.so \
  ./ConanSandboxServer.sh -log ...your usual flags...
```

- It reaches the bridge at `http://127.0.0.1:3010`, so run the bridge on the same host as the
  server. With Docker, start the bridge container with `--network container:<conan container>`.
  Set `TAKARO_CONAN_BRIDGE_URL` in the server's environment if you changed `httpPort`.
- It logs to `ConanSandbox/Saved/Logs/TakaroConanNative.log`. On any server build other than
  25639945 it writes why it stayed off and changes nothing. `TAKARO_CONAN_NATIVE_DISABLE=1` turns
  it off.
- Without it, Takaro messages fail with a clear error instead of appearing in chat.

Then start the bridge:

```bash
npm start
```

### 5. Check that it worked

Ask the bridge's own health endpoint on the server host:

```bash
curl http://127.0.0.1:3010/health
```

It reports the connection state, the `gameServerId` Takaro assigned, and under `target` the server
build this package was built for. With the native library running, `modBridge.connected` is `true`
and `modBridge.lastPollSource` is `TakaroConan-native`. If the registration token was rejected, `/health` shows
`takaroIdentifyError` and the bridge stops retrying until you fix the token.

The bridge logs the same identity on its first line, which is the quickest way to tell two installs
apart:

```text
Takaro target: linux-25639945 (<fingerprint>) revision 25639945 connector 1.0.2 source <commit>
```

And in Takaro, the game server shows as **online** and lists your online players. If it stays
offline, check, in order: the `registrationToken` in `TakaroConfig.txt`, that RCON accepts your
password, and that the bridge process is still running.

### 6. Upgrading

Stop the bridge. Unzip the new version over the old folder, or into a new folder and copy your
`TakaroConfig.txt` across — the config is not part of the zip, so it survives. Run
`npm ci --omit=dev` again, then start the bridge. Restart the Conan server only if
`native/libtakaro-conan-native.so` changed; the server loads it at start.

## What works, what doesn't

Status below comes from the recorded capability data and the live checks run on **2026-06-20** and
**2026-06-21** against a real Conan Exiles Enhanced dedicated server with Enhanced Pippi and one
real player connected, and a real-client re-check on **2026-10-02** against the pinned build
25639945: a Conan Exiles Enhanced client (revision 378,132) on a Windows PC joined an isolated
server without mods, and every row marked "2026-10-02" was driven through Takaro and seen in
that client. The chat rows marked "native" were checked the same way on 2026-10-02 with the
native library preloaded and no mods on the server or the client.
Anything that was never exercised in a live test says so.
✅ = works, ⚠️ = works with a caveat or is unproven, ❌ = does not work.

The bridge's own end-to-end test suite re-runs the protocol rows on every build against a fake
Conan RCON server and a fake Takaro: identify, reachability, the player list, a console command,
the chat refusal, reconnect after a dropped socket and shutdown. That is a protocol proof, not a
gameplay one — a row that needs a real client, a real player or Enhanced Pippi stays ⚠️ below
until somebody checks it in game.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The bridge registers with Takaro and reachability checks succeed against the live server. Re-proven on build 25356024: the bridge identified with hosted Takaro and `testReachability` came back `connectable: true`. |
| Server restart / reconnect | ⚠️ | Reconnect after a dropped socket is proven — the bridge test closes the socket with `1001` and the bridge identifies again, and on build 25356024 it recovered from three real `1006` closes during startup. Recovery after a *Conan* restart is still unverified. |
| Player list | ✅ | Names and Steam64 ids, read from Conan's `listplayers`. This is how Takaro loads players for Conan. Re-proven empty on build 25356024; the populated case is the June live check. |
| Single player lookup | ✅ | Looking up one player by their game id returns the same data as the player list. |
| Player location | ⚠️ | Only with `databasePath` set: coordinates are read from Conan's save database. Without it, Takaro gets `0,0,0`. |
| Player inventory | ⚠️ | Only with `databasePath` set: read from the save database. Without it, the inventory comes back empty. |
| Item catalogue | ⚠️ | Not a real catalogue — it lists only the item ids that already exist in the save database, and only with `databasePath` set. |
| Entity catalogue | ⚠️ | Same: only the creature/actor classes already present in the save database, and only with `databasePath` set. |
| Locations / points of interest | ⚠️ | Returns saved player character positions, not real Conan points of interest, and only with `databasePath` set. |
| Broadcast a message | ✅ | Native, Linux server only. 2026-10-02: `[Takaro]: …` appeared as a normal line in every connected client's chat feed, with accents and symbols intact and the Takaro sender name override applied. Needs at least one player online. |
| Whisper a player | ✅ | Native, Linux server only. 2026-10-02: a message to the player's Steam64 id showed only in that client's chat. A player who is not online gets "Recipient … is not online" and nothing is sent. |
| Give an item | ✅ | Spawns the item through Conan's admin relay; the player must be **online**. 2026-10-02: 7 Stone (`10001`) appeared in the client's inventory. |
| Teleport a player | ✅ | `TeleportPlayer` through Conan's admin relay, raw world units, **online** players only. 2026-10-02: the client moved to the requested spot. |
| Run a console command | ✅ | Commands are sent over RCON and the raw output comes back to Takaro. 2026-10-02: `broadcast …` through Takaro showed a "Server admin message" popup in the client. |
| Kick a player | ✅ | 2026-10-02: the client got "Kicked from Server" with the Takaro reason. Kicking a player who is not online fails with Conan's "No player with platform ID" answer. |
| Ban a player (timed and permanent) | ⚠️ | **Online players only.** 2026-10-02: banning a connected player kicked them, wrote `blacklist.txt`, and their rejoin was refused ("User is banned from this server"). Conan answers a ban of an offline player with "No player with platform ID" and bans nothing; the bridge reports that as a failed action. Timed bans were not checked. |
| Unban a player | ✅ | `unbanplayer <steam64>` works for offline players. 2026-10-02: after the unban the same client rejoined. |
| Ban list | ✅ | Reads Conan's `listbans`, which on build 25639945 prints one bare Steam ID per line (no reason). 2026-10-02: populated after a ban, empty after the unban. |
| Shut the server down | ✅ | Proven on build 25356024: Takaro's shutdown reached the server over RCON and the server process exited cleanly (code 0). It is **slow** — about four and a half minutes of unloading and saving between the command and `LogExit: Exiting.`, with no output for most of it. Do not assume it failed. |
| Player joined event | ✅ | Derived from changes in the player list, so it can lag by up to one poll (10 s by default). 2026-10-02: arrived in Takaro for each client join. |
| Player left event | ✅ | Same as joins: derived from the player list. 2026-10-02: arrived for a kick, a ban and a client timeout. |
| Player chat event | ✅ | 2026-10-02 on build 25639945 **without any mod**: vanilla `ChatWindow` log lines reached Takaro as `chat-message` with the player attached, and `@`-prefixed Takaro commands typed in game ran. |
| Player death event | ⚠️ | Best effort, parsed out of Conan's log lines. No live death was captured in a test. |
| Entity kill event | ⚠️ | Best effort from the same log lines, with the killer resolved only if they are online. No live kill was captured. |
| Log events | ⚠️ | Log tailing works against real Conan logs, but Takaro does not store server log lines as searchable events. |
| Map info | ⚠️ | The bridge answers with an empty/disabled map; Conan exposes no map metadata. |
| Map tiles | ❌ | The Takaro API does not support map tiles for Generic-connector servers. Nothing on the game server side changes that. |
| Discord chat bridge | ⚠️ | Game → Discord ✅ 2026-10-02: the chatBridge module relayed in-game chat and join/leave posts to Discord. Discord → game uses the native chat path above; not yet checked with a real Discord post. |
| Shop & economy | ⚠️ | Never tested for Conan. Item delivery would go through the same online-player-only spawn route as "Give an item". |

### Known issues

- **Chat needs the native library, on Linux, on build 25639945.** Conan has no command that writes
  a normal chat line (`broadcast` shows a screen overlay), and no mod loads on this build. On a
  Windows server or another build, Takaro messages fail with a clear error.
- **Half the read features need the save database.** Location, inventory, and the item, entity and
  location lists are empty unless `databasePath` points at Conan's `game_0.db` on the same host.
- **Give item and teleport only work on online players.** They go through Conan's admin relay to a
  connected client; offline players cannot be targeted.
- **Conan's RCON throttles you.** Conan's karma system charges every new RCON connection. The
  bridge keeps one connection open and reuses it; before it did, the default 10 s poll alone
  tripped karma after about an hour on build 25639945 and locked out every RCON action, shutdown
  included, for ten minutes at a time. Running several tools against the same server at once
  still trips it.
- **No map.** Takaro's API does not support map tiles for Generic-connector servers.
- **Shutdown takes minutes.** On build 25356024 the server closed its net driver
  immediately, then spent about four and a half minutes unloading and saving — logging
  nothing for most of it — before exiting. Give it five minutes before treating a Takaro
  shutdown as failed, and set any stop timeout around it accordingly.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
