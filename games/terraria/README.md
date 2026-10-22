# Takaro Terraria Connector

Connects a Terraria dedicated server running **TShock 6.2.1** (Terraria **1.4.5.8**) to Takaro.
It has two server-side parts: a TShock plugin and a bridge service that runs next to the server.
Players do not install anything.

Install both parts. The bridge alone cannot report deaths, NPC kills, player positions or
inventories, give items, teleport players or make bans that hold; the plugin alone cannot reach
Takaro.

## Install

### 1. Before you start

You need:

- A **Terraria 1.4.5.8 dedicated server running TShock 6.2.1** (Linux or Windows) that you can
  stop, start and copy files to. TShock must have been started once, so that its `tshock/` folder
  with `tshock/config.json` exists.
- **Node.js 22** on the same machine, to run the bridge. The bridge talks to TShock on
  `127.0.0.1` and reads TShock's log files, so it must run on the server machine itself.
- A tool to unpack `.zip` files.
- A **Takaro** account and your Takaro **registration token**. Nothing needs to be port-forwarded:
  the bridge connects out to `wss://connect.takaro.io/`.

Players need a Terraria client of the same version as the server (1.4.5.8). A newer client is
refused at join time with `You are not using the same version as this server.`

The exact server build this connector is maintained for:

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `tshock-v6.2.1` | v6.2.1 | tshock | — | None | maintained | contract |
<!-- takaro-maint:targets:end -->

### 2. Download

Download the latest release from <https://takaro.io/connectors/terraria>. You need both files;
their names carry the TShock version they are built for and the connector version:

- **`takaro-terraria-plugin-tshock-v6.2.1-<version>.zip`**: the TShock plugin
- **`takaro-terraria-bridge-tshock-v6.2.1-<version>.zip`**: the bridge

Do not use a `dev` or `pr-` build; those are untested.

### 3. Copy it into place

Stop the server.

**Plugin.** The plugin zip holds one folder, `TakaroTerrariaEvents/`. Copy the
**`TakaroTerrariaEvents.dll`** inside it (the file, not the folder) into TShock's plugin folder:

```
<server>/ServerPlugins/TakaroTerrariaEvents.dll
```

**Bridge.** The bridge zip holds one folder, `TakaroTerrariaBridge/`. Unpack it into the server
folder, so that it sits next to the `tshock/` folder:

```
<server>/
    ServerPlugins/TakaroTerrariaEvents.dll
    tshock/
    TakaroTerrariaBridge/
        dist/
        node_modules/
        package.json
        ...
```

Everything the bridge needs is in the zip; there is nothing to install with `npm`. If you put
the folder anywhere else, set `logFiles` and `mapDir` (step 4) to absolute paths.

### 4. Configure

**A TShock REST token for the bridge.** The bridge drives TShock through its REST API. In
`<server>/tshock/config.json`, inside `"Settings"`, set `RestApiEnabled` to `true`, keep
`RestApiPort` at `7878`, and add a token to `ApplicationRestTokens`. Use a long random string of
your own as the token:

```json
"RestApiEnabled": true,
"RestApiPort": 7878,
"ApplicationRestTokens": {
  "<a long random token>": {
    "Username": "takaro-rest",
    "UserGroupName": "superadmin"
  }
},
```

- The group must have the **`takaro.admin`** permission (`superadmin` has every permission).
  Without it teleport fails, player positions read `0,0,0` and inventories come back empty, all
  without an error.
- Keep the user name **`takaro-rest`**. TShock writes a log line for every REST call
  (`takaro-rest executed: ...`), and the bridge skips its own lines by that name. With another
  name, add `<that name> executed:` to `logExcludePatterns` below, or those lines flood Takaro.

Start the server (TShock reads `config.json` at start).

**The bridge.** Start it from inside `TakaroTerrariaBridge/` and leave it running (in a terminal,
or as a service):

```bash
cd <server>/TakaroTerrariaBridge
npm start
```

On its first start the bridge creates **`<server>/TakaroConfig.txt`** (next to the
`TakaroTerrariaBridge/` folder, not inside it) with a new `identityToken`, and shows a banner that
names the file. Open it, paste your Takaro registration token after `registrationToken=` and the
REST token from `config.json` after `tshockToken=`, and save. Saving is enough: the bridge reads
the file every few seconds and connects, no restart needed.

This is the file with its defaults. When the bridge creates it, `identityToken` and `serverName`
are already filled in. You can also write it yourself before the first start; leave
`identityToken` empty and the bridge fills it in.

```
# Takaro Terraria bridge settings. The bridge reads this file every few seconds:
# paste the registration token and save, no restart needed.
# identityToken is this server's identity in Takaro; the bridge fills it in when empty.
# An empty serverName means "Terraria Server (<first 8 characters of identityToken>)".
# Relative paths (logFiles, mapDir) start at the folder holding this file: the server folder.

# Takaro connection
registrationToken=
identityToken=
serverName=
serverChatName=Takaro
takaroWsUrl=wss://connect.takaro.io/

# TShock REST API (RestApiEnabled in tshock/config.json). Instead of a token you may set
# tshockUsername= and tshockPassword=.
tshockBaseUrl=http://127.0.0.1:7878
tshockToken=

# Local status server (GET /health, GET /coverage) and player poll interval. Restart to apply.
httpPort=3020
pollIntervalMs=10000

# The TShock log DIRECTORY to tail (a directory follows TShock's log rotation), and the
# lines never sent to Takaro (the bridge's own REST calls). Restart to apply.
logFiles=tshock/logs
logExcludePatterns=takaro-rest executed:,RestManager:

# Console commands Takaro may run, by exact match and by prefix. Shutdown is off by default.
commandAllowlistExact=help,/help
commandAllowlistPrefixes=say,time
enableShutdown=false

# The world map: the plugin renders the loaded world into mapDir (<TShock folder>/takaro) and
# the bridge shows it on Takaro's map. protocolVersion=1 speaks Takaro protocol 1 when Takaro
# offers it (the map, events resent after a reconnect); 0 stays on version 0. Restart to apply.
# The plugin renders the map at world load, every TAKARO_MAP_INTERVAL_MINUTES (environment of
# the TShock server: default 30, 0 = off) and when an admin runs /takaromap.
mapDir=tshock/takaro
protocolVersion=1
```

- `serverName` is the name the server gets in Takaro and must be unique among your Takaro game
  servers.
- `TAKARO_REGISTRATION_TOKEN`, `TSHOCK_TOKEN`, `TSHOCK_USERNAME` and `TSHOCK_PASSWORD` in the
  bridge's environment override the file and are never written to it.
- `logFiles` is the TShock log **folder**, not one file: TShock starts a new log file on every
  start, and the bridge follows it. Chat, joins and leaves, deaths and kills all come from there.
- `enableShutdown=true` lets Takaro stop the server; it is off by default.
- `TAKARO_MAP_INTERVAL_MINUTES` goes in the **TShock server's** environment, not in this file: how
  often the plugin redraws the world map (default 30, `0` draws it only when the world loads). An
  admin can redraw it at any time with `/takaromap`.

Keep the registration token and the REST token private.

### 5. Check that it worked

In the TShock log or console:

```
Takaro Terraria Events plugin loaded (<version>)
```

In the bridge's output:

```
Terraria bridge health: http://127.0.0.1:3020/health
Identified successfully with Takaro (gameServerId=...)
```

The game server now shows in Takaro under the `serverName` from the config file, as online. You
can also ask the bridge:

```bash
curl http://127.0.0.1:3020/health
```

`takaroIdentified` and `tshockReachable` must both be `true`.

If a token is missing, the bridge shows a banner instead that names the file and the line to fill
in (`RegistrationToken not set, the server is not connected to Takaro.` or
`tshockToken not set, the bridge cannot reach the TShock REST API.`). A wrong registration token
shows `Takaro rejected identify: Invalid registrationToken provided`; a `serverName` that another
of your game servers already uses is reported too. Fix the line in `TakaroConfig.txt` and save;
the bridge reconnects within a few seconds.

### 6. Upgrading

Stop the bridge and the server. Replace `<server>/ServerPlugins/TakaroTerrariaEvents.dll` with
the one from the new plugin zip, delete the `TakaroTerrariaBridge/` folder and unpack the new one
in its place. Start the server, then the bridge. `TakaroConfig.txt` next to the folder is not
touched, so the server keeps its token, settings and identity in Takaro. Always upgrade both
parts together.

## What works, what doesn't

Tested on 2026-10-22 against **TShock 6.2.1 on Terraria 1.4.5.8** (the `tshock-v6.2.1` target),
with the 0.6.0 release and then the build that becomes the next release, on Takaro's staging
service, with a Terraria 1.4.5.8 network client playing on the server. Fighting was not tested.
More detail on every caveat: [KNOWN-ISSUES.md](KNOWN-ISSUES.md).
✅ = works, ⚠️ = works with a caveat or not tested, ❌ = not available.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The bridge connects out to Takaro; the server shows online. |
| Reconnects after a restart | ✅ | After a server or bridge restart the bridge reconnects on its own and events resume. |
| Config change without a restart | ✅ | Saving `TakaroConfig.txt` with a token connects within a few seconds. |
| Player list | ✅ | Character names, as players join and leave. |
| Single player lookup | ✅ | Takaro looks the player up by name, for example when they run a command. |
| Player location | ✅ | The player's position in world pixels (16 per tile). |
| Player inventory | ✅ | The items and amounts the player carries. |
| Item catalogue | ⚠️ | All 6147 items; giving by name works, but some names read wrong (`Bandof Regeneration`). |
| Entity catalogue | ❌ | Terraria has no list of creatures to offer; Takaro gets an empty list. |
| Locations / points of interest | ❌ | Terraria has no named locations; Takaro gets an empty list. |
| Run a console command | ✅ | Only allowlisted commands run (by default `help`, `say …`, `time …`); others are refused. |
| Broadcast a message | ✅ | Shown to every player as a server broadcast. |
| Whisper a player | ✅ | Only that player sees it. |
| Give an item | ✅ | Straight into the inventory. Refused when it does not fit, instead of dropping items. |
| Teleport a player | ✅ | To world pixel coordinates; a player put in the air falls to the ground. |
| Kick a player | ✅ | The player sees the reason. |
| Ban a player | ✅ | Timed and permanent. A banned player who rejoins sees the reason and the time left. |
| Unban a player | ✅ | The player can rejoin at once. |
| Ban list | ✅ | Bans made in the game show up in Takaro's ban list after its ban sync. |
| Shut the server down | ✅ | Saves the world and stops the server. Off by default: set `enableShutdown=true` to allow it. |
| Player joined event | ✅ | Within about a second of joining. |
| Player left event | ✅ | On leaving and on a kick. |
| Player chat event | ✅ | The player and the exact text. Messages starting with `/` are TShock commands and never reach Takaro. |
| Player death event | ✅ | The player and Terraria's death message. |
| Entity kill event | ⚠️ | The NPC's name and the killer's held item as the weapon (a guess). Kills by town NPCs, traps or lava are not sent. Player kills not re-tested. |
| Log events | ⚠️ | Sent, but Takaro does not keep them as events. |
| World map | ⚠️ | The whole world, drawn by the plugin. Works on Takaro's staging service; not yet on takaro.io. |
| Modules: chat commands | ✅ | `@` commands work; replies are whispered to the player. |
| Modules: teleports | ✅ | `@settp` and `@tp` move the player. |
| Discord: game chat → Discord | ⚠️ | Takaro posts chat, joins and leaves, and Discord accepts each post; not checked in the Discord app. |
| Discord: Discord → game chat | ✅ | A Discord message is broadcast in game as `[D] <name>: <text>`. |
| Shop & economy | ✅ | `@balance` and `@shop` work; a purchase lands straight in the inventory. |

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
