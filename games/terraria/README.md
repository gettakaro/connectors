# Takaro Terraria Connector

Connects a Terraria server running **TShock** (Linux or Windows) to Takaro with two server-side
parts: a TShock plugin and a bridge that runs next to the server. Install both. Players do not
install anything.

## Install

You need: a Terraria server with TShock in the version below (started once, so
`tshock/config.json` exists), **Node.js 22** on the same machine, and a Takaro game server of type
**Generic** with its registration token.

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `tshock-v6.2.1` | v6.2.1 | tshock | — | None | maintained | contract |
<!-- takaro-maint:targets:end -->

### 1. Download

From https://takaro.io/connectors/terraria, download both files (`<target>` is the target above,
e.g. `tshock-v6.2.1`):

- `takaro-terraria-plugin-<target>-<version>.zip` (the TShock plugin)
- `takaro-terraria-bridge-<target>-<version>.zip` (the bridge)

### 2. Install

Stop the server, then:

- **Plugin:** copy `TakaroTerrariaEvents/TakaroTerrariaEvents.dll` (the file, not the folder) into
  `<server>/ServerPlugins/`.
- **Bridge:** unpack `TakaroTerrariaBridge/` into the server folder, next to `tshock/`. Do not run
  `npm install`.

```text
<server>/ServerPlugins/TakaroTerrariaEvents.dll
<server>/tshock/
<server>/TakaroTerrariaBridge/
```

### 3. Configure

**TShock REST token.** In `<server>/tshock/config.json`, inside `"Settings"`, turn on the REST API
and add a long random token of your own, then start the server:

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

Keep the user name `takaro-rest`. A group other than `superadmin` needs the `takaro.admin` and
`tshock.broadcast` permissions.

**Bridge.** Start it and keep it running (in a terminal or as a service):

```bash
cd <server>/TakaroTerrariaBridge
npm start
```

It creates `<server>/TakaroConfig.txt` (next to the bridge folder, not inside it). Fill in both
tokens and save; it connects within a few seconds, no restart needed:

```ini
registrationToken=your-registration-token-here
tshockToken=<the REST token from config.json>
```

| Optional setting | What it does |
|---|---|
| `serverName` | Server name in Takaro. |
| `serverChatName` | Sender name on messages from Takaro, used when none is set for the server in Takaro (default `Takaro`). |
| `commandAllowlistExact`, `commandAllowlistPrefixes` | Console commands Takaro may run. |
| `enableShutdown` | `true` lets Takaro stop the server. |

### 4. Check that it works

The TShock console shows `Takaro Terraria Events plugin loaded (<version>)`, the bridge shows
`Identified successfully with Takaro`, and the server shows as online in Takaro. A missing token
shows a banner naming the line to fill in.

**Upgrading:** upgrade both parts together: replace the `.dll`, replace the
`TakaroTerrariaBridge/` folder, keep `TakaroConfig.txt`.

## What works, what doesn't

Checked on TShock 6.2.1 (Terraria 1.4.5.8), 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects after a server or bridge restart. |
| Player list | ✅ | Character names. |
| Player location | ✅ | In world pixels (16 per tile). |
| Teleport | ✅ | |
| Player inventory | ✅ | |
| Item list | ⚠️ | Some names read wrong (`Bandof Regeneration`). |
| Entity list | ❌ | Terraria has no creature list. |
| Locations | ❌ | Terraria has no named locations. |
| World map | ⚠️ | Drawn by the plugin. Not checked yet. |
| Send a message | ✅ | Broadcasts and whispers, sent as the server chat name. |
| Console commands | ✅ | Allowlisted commands only. |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent. |
| Ban list | ✅ | |
| Give an item | ✅ | Refused when it does not fit, or while the player is dead. |
| Shop | ✅ | |
| Shut down the server | ✅ | Needs `enableShutdown=true`. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | `/` commands are not sent. |
| Player death | ✅ | |
| Entity killed | ⚠️ | Kills by town NPCs, traps or lava are not sent. |
| Server log lines | ⚠️ | Takaro does not keep them. |
| Discord chat bridge | ⚠️ | Discord to game works; game to Discord not checked. |
| Modules | ✅ | |
| Game name, version and icon | ✅ | |

## Known issues

- Players are known by character name only: renaming a character makes a new player in Takaro.
- Players need a client of the same Terraria version as the server.
- Takaro shows a timed ban as permanent after its ban sync; the ban still ends on time.
- More detail: [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
