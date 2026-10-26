# Takaro Terraria Connector

Connects a Terraria **1.4.5.8** dedicated server running **TShock 6.2.1** to Takaro. It has two
server-side parts that are both required: a TShock plugin and a bridge program that runs next to
the server. Players do not install anything.

## Install

### 1. Before you start

- A Terraria 1.4.5.8 server with TShock 6.2.1 (Linux or Windows), started once so `tshock/config.json` exists.
- **Node.js 22** on the same machine, for the bridge.
- A Takaro game server and its **registration token**. Nothing needs port-forwarding; the bridge
  connects out to `wss://connect.takaro.io/`.

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `tshock-v6.2.1` | v6.2.1 | tshock | — | None | maintained | contract |
<!-- takaro-maint:targets:end -->

Players need a Terraria client of the same version as the server.

### 2. Download

From https://takaro.io/connectors/terraria (or the `terraria-v<version>` release on
[GitHub](https://github.com/gettakaro/connectors/releases)), download both:

- `takaro-terraria-plugin-tshock-v6.2.1-<version>.zip` (the TShock plugin)
- `takaro-terraria-bridge-tshock-v6.2.1-<version>.zip` (the bridge)

### 3. Copy it into place

Stop the server. Copy `TakaroTerrariaEvents.dll` from the plugin zip into `<server>/ServerPlugins/`.
Unpack the bridge zip into the server folder, so you get `<server>/TakaroTerrariaBridge/` next to
`<server>/tshock/`. The bridge needs no `npm install`.

### 4. Turn on the TShock REST API

In `<server>/tshock/config.json`, under `"Settings"`, add a REST token of your own (a long random string):

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

Keep the user name `takaro-rest`. If you use another group than `superadmin`, it needs the
`takaro.admin` permission, or teleport, locations and inventories silently fail. Start the server.

### 5. Start the bridge and set the tokens

```bash
cd <server>/TakaroTerrariaBridge
npm start
```

Keep it running (in a terminal or as a service). On first start it creates
`<server>/TakaroConfig.txt`:

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

Paste your Takaro registration token after `registrationToken=` and the REST token from step 4
after `tshockToken=`, and save. The bridge connects within a few seconds; no restart needed.
`serverName` must be unique among your Takaro game servers. The environment variables
`TAKARO_REGISTRATION_TOKEN` and `TSHOCK_TOKEN` override the file.

### 6. Check that it worked

The TShock console shows `Takaro Terraria Events plugin loaded (<version>)`, the bridge shows
`Identified successfully with Takaro`, and Takaro shows the server as online.
`curl http://127.0.0.1:3020/health` must report `takaroIdentified` and `tshockReachable` as `true`.
A missing or wrong token makes the bridge print a banner naming the line to fix.

### Updating

Stop the bridge and the server. Replace `ServerPlugins/TakaroTerrariaEvents.dll`, delete
`TakaroTerrariaBridge/` and unpack the new one. Start the server, then the bridge.
`TakaroConfig.txt` is kept. Always update both parts together.

## What works, what doesn't

Tested on TShock 6.2.1 (Terraria 1.4.5.8) on 2026-10-26.

| Feature | Status | Note |
|---|---|---|
| Connection and reconnect | ✅ | Reconnects by itself after a server or bridge restart. |
| Game profile | ✅ | Terraria name, icon, game version and connector version. |
| Player list and lookup | ✅ | Character names; Terraria sends no Steam id. |
| Player location | ✅ | In world pixels (16 per tile). |
| Player inventory | ✅ | |
| Item catalogue | ⚠️ | All items; some names read wrong (`Bandof Regeneration`). |
| Entity catalogue | ❌ | Terraria has no creature list to offer. |
| Locations | ❌ | Terraria has no named locations. |
| World map | ✅ | The whole world, redrawn every 30 minutes or with `/takaromap`. |
| Broadcast and whisper | ✅ | A whisper is seen only by that player. |
| Give an item | ✅ | Refused when it does not fit in the inventory. |
| Teleport a player | ✅ | To world pixel coordinates. |
| Run a console command | ✅ | Only allowlisted commands; others are refused. |
| Kick a player | ✅ | |
| Ban, unban, ban list | ✅ | Timed bans expire by themselves. |
| Shut the server down | ✅ | Off by default; set `enableShutdown=true`. |
| Player joined / left | ✅ | |
| Chat | ✅ | `/` commands are TShock's and never reach Takaro. |
| Player death | ✅ | With Terraria's death message. |
| Entity kill | ⚠️ | Weapon is the held item; trap and lava kills are not sent. |
| Log events | ⚠️ | Sent, but Takaro does not store them. |
| Modules, economy and shop | ✅ | Purchases go straight into the inventory. |
| Discord: game to Discord | ⚠️ | Discord accepts each post; not checked in the Discord app. |
| Discord: Discord to game | ✅ | Use the `chatBridgeNoEcho` module. |

## Known issues

- Timed bans made in game show as permanent in Takaro's ban list (a Takaro issue).
- More detail on each caveat: [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
