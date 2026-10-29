# Takaro Terraria Connector

Connects a Terraria dedicated server running TShock (Linux or Windows) to Takaro. It has two
server-side parts: a TShock plugin and a bridge that runs next to the server. Install both: the
bridge alone cannot report deaths, kills, positions or inventories, give items, teleport or make
bans that hold; the plugin alone cannot reach Takaro. Players do not install anything.

## Install

### 1. Before you start

- A Terraria dedicated server running **TShock** (Linux or Windows), in the version the release
  is built for (see step 2), that you can stop, start and copy files to. Start TShock once, so its
  `tshock/` folder with `tshock/config.json` exists.
- **Node.js 22** on the same machine, to run the bridge. The bridge talks to TShock on
  `127.0.0.1` and reads TShock's log files, so it must run on the server machine.
- A tool to unpack `.zip` files.
- A Takaro game server of type **Generic** and its **registration token**. Nothing needs to be
  port-forwarded: the bridge connects out to `wss://connect.takaro.io/`.

Players need a Terraria client of the same version as the server. A newer client is refused with
`You are not using the same version as this server.`

### 2. Download

Download the latest release from https://takaro.io/connectors/terraria. The server build it is
made for:

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `tshock-v6.2.1` | v6.2.1 | tshock | — | None | maintained | contract |
<!-- takaro-maint:targets:end -->

You need both files (`<target>` is the target above, e.g. `tshock-v6.2.1`):

- `takaro-terraria-plugin-<target>-<version>.zip`: the TShock plugin
- `takaro-terraria-bridge-<target>-<version>.zip`: the bridge

Each release has a `SHA256SUMS` file to check the downloads with. Do not use a `dev` or `pr-`
build; those are untested.

### 3. Install

Stop the server.

- **Plugin:** the plugin zip holds a folder `TakaroTerrariaEvents/`. Copy the
  `TakaroTerrariaEvents.dll` inside it (the file, not the folder) into `<server>/ServerPlugins/`.
- **Bridge:** the bridge zip holds a folder `TakaroTerrariaBridge/`. Unpack it into the server
  folder, next to `tshock/`. Everything it needs is in the zip; do not run `npm install`.

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

If you put the bridge folder anywhere else, set `logFiles` and `mapDir` (step 4) to absolute
paths.

### 4. Configure

**TShock REST token.** The bridge drives TShock through its REST API. In
`<server>/tshock/config.json`, inside `"Settings"`, set `RestApiEnabled` to `true`, keep
`RestApiPort` at `7878`, and add a long random token of your own to `ApplicationRestTokens`:

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

- The group needs the **`takaro.admin`** permission (`superadmin` has all). Without it teleport
  fails, locations go missing and inventories come back empty, all without an error.
- Keep the user name **`takaro-rest`**: the bridge skips TShock's `takaro-rest executed: ...` log
  lines by that name. With another name, add `<that name> executed:` to `logExcludePatterns`, or
  those lines flood Takaro.
- If you do not use `superadmin`, also grant **`tshock.broadcast`** so Takaro messages are written
  to the server console too. Players get them either way.

Start the server (TShock reads `config.json` at start).

**Bridge.** Start it from inside `TakaroTerrariaBridge/` and keep it running (in a terminal or as
a service):

```bash
cd <server>/TakaroTerrariaBridge
npm start
```

On its first start the bridge creates `<server>/TakaroConfig.txt` (next to the
`TakaroTerrariaBridge/` folder, not inside it) and shows a banner naming the file. Paste your
Takaro registration token after `registrationToken=` and the REST token after `tshockToken=`, and
save. No restart is needed: the bridge rereads the file every few seconds and connects. You can
also write the file yourself before the first start; leave `identityToken` empty and the bridge
fills it in. The file with its defaults:

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

- `serverName` is the name in Takaro and must be unique among your Takaro game servers.
- `serverChatName` is the sender players see on messages from Takaro when Takaro sends no chat
  name of its own; the chat name set in Takaro wins. Needs the plugin from the same release and a
  REST user with `takaro.admin`.
- `TAKARO_REGISTRATION_TOKEN`, `TAKARO_SERVER_CHAT_NAME`, `TSHOCK_TOKEN`, `TSHOCK_USERNAME` and `TSHOCK_PASSWORD` in the
  bridge's environment override the file and are never written to it.
- `logFiles` is the TShock log **folder**, not one file; the bridge follows TShock's new log file
  on every start. Chat, joins, leaves, deaths and kills come from there.
- `enableShutdown=true` lets Takaro stop the server; it is off by default.
- `TAKARO_MAP_INTERVAL_MINUTES` goes in the **TShock server's** environment, not in this file: how
  often the plugin redraws the world map (default 30, `0` = only at world load). An admin can
  redraw it any time with `/takaromap`.

Keep the registration token and the REST token private.

### 5. Check that it works

The TShock log or console shows:

```
Takaro Terraria Events plugin loaded (<version>)
```

The bridge's output shows:

```
Terraria bridge health: http://127.0.0.1:3020/health
Identified successfully with Takaro (gameServerId=...)
```

The game server now shows as online in Takaro under its `serverName`. You can also run
`curl http://127.0.0.1:3020/health`: `takaroIdentified` and `tshockReachable` must both be `true`.

A missing token shows a banner naming the file and the line to fill in
(`RegistrationToken not set, the server is not connected to Takaro.` or
`tshockToken not set, the bridge cannot reach the TShock REST API.`). A wrong registration token
shows `Takaro rejected identify: Invalid registrationToken provided`; a `serverName` already used
by another of your game servers is reported too. Fix the line in `TakaroConfig.txt` and save; the
bridge reconnects within a few seconds.

### 6. Upgrade

Always upgrade both parts together. Stop the bridge and the server, replace
`<server>/ServerPlugins/TakaroTerrariaEvents.dll` with the one from the new plugin zip, delete the
`TakaroTerrariaBridge/` folder and unpack the new one in its place. Start the server, then the
bridge. `TakaroConfig.txt` is not touched, so the server keeps its token, settings and identity.

## What works, what doesn't

Checked on TShock 6.2.1 (Terraria 1.4.5.8), 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects by itself after a server or bridge restart. |
| Player list | ✅ | Character names. |
| Player location | ✅ | In world pixels (16 per tile). |
| Teleport | ✅ | To world pixel coordinates; a player put in the air falls to the ground. |
| Player inventory | ✅ | The items and amounts the player carries. |
| Item list | ⚠️ | All items; giving by name works, but some names read wrong (`Bandof Regeneration`). |
| Entity list | ❌ | Terraria has no list of creatures; Takaro gets an empty list. |
| Locations | ❌ | Terraria has no named locations; Takaro gets an empty list. |
| World map | ⚠️ | The whole world, drawn by the plugin. Not checked on takaro.io yet. |
| Send a message | ✅ | A broadcast (`<chat name>: message`) or a whisper (`<From <chat name>> message`); the chat name comes from Takaro. |
| Console commands | ✅ | Only allowlisted commands (by default `help`, `say …`, `time …`). |
| Kick | ✅ | The player sees the reason. |
| Ban and unban | ✅ | Timed and permanent; a banned player sees the reason and time left. Unbanned players can rejoin at once. |
| Ban list | ✅ | Bans made in game show up after Takaro's ban sync. |
| Give an item | ✅ | Straight into the inventory; refused when it does not fit, instead of dropping items. |
| Shop | ✅ | A purchase lands straight in the inventory. |
| Shut down the server | ✅ | Saves the world and stops. Off by default: set `enableShutdown=true`. |
| Player joined / left | ✅ | Also on a kick. |
| Chat messages | ✅ | Messages starting with `/` are TShock commands and never reach Takaro. |
| Player death | ✅ | With Terraria's death message. |
| Entity killed | ⚠️ | NPC name, with the killer's held item as the weapon (a guess). Kills by town NPCs, traps or lava are not sent. Player kills and fights are not checked. |
| Server log lines | ⚠️ | Sent, but Takaro does not keep them as events. |
| Discord chat bridge | ⚠️ | Discord messages show in game as `[D] <name>: <text>`. Game chat, joins and leaves are posted to Discord, but not checked in the Discord app. |
| Modules | ✅ | `@` commands (replies are whispered) and teleports (`@settp`, `@tp`). |
| Game name, version and icon | ✅ | Takaro shows the server as Terraria with its icon, build and connector version, where Takaro supports game profiles. |

## Known issues

- Players are known by character name only: renaming a character makes a new player in Takaro.
- Takaro shows a timed ban as permanent after its ban sync; the ban on the server still ends on time.
- More detail on every limit: [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
