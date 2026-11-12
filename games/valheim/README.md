# Takaro Valheim Connector

A BepInEx plugin that runs inside a Valheim dedicated server. Players join with plain Valheim;
only reading a player's inventory needs an optional mod on that player's game.

## Install

You need: a Valheim dedicated server (built and tested for Linux) with **BepInExPack Valheim
5.4.2351** (`denikson/BepInExPack_Valheim` on Thunderstore), started once so `BepInEx/plugins/`
exists, and a Takaro game server of type **Generic** with its registration token.

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `linux-1.0.21` | 1.0.21 | linux | — | None | candidate | contract |
<!-- takaro-maint:targets:end -->

### 1. Download

From https://takaro.io/connectors/valheim, download **`takaro-valheim-plugin.zip`**. Do not use the
`valheim-dev` pre-release.

### 2. Install

Stop the server and unzip so the whole `TakaroValheim` folder lands in `BepInEx/plugins/`:

```text
<server>/BepInEx/plugins/TakaroValheim/TakaroValheim.dll
<server>/BepInEx/plugins/TakaroValheim/...the other files from the zip
```

**Optional inventory mod.** A player who wants Takaro to see their inventory installs BepInExPack
Valheim 5.4.2351 in their game folder, then unzips `takaro-valheim-inventory-companion.zip` (same
release) into its `BepInEx/plugins/`. Other players play normally without it.

### 3. Configure

Start the server; it creates `BepInEx/config/com.takaro.valheim.cfg`. Paste your token and save; it
connects within a few seconds, no restart needed:

```ini
## Takaro Valheim connector settings. Saving this file applies the token, identity,
## serverName and takaroWsUrl within a few seconds; other settings need a server restart.

[Takaro]

## Takaro registration token: paste it here and save; the connector connects within a few seconds.
registrationToken = your-registration-token-here

## The server name shown in Takaro; must be unique in your Takaro domain. Empty: the server's own name plus the start of identityToken.
serverName =

## Identifies this server in Takaro. Filled in by the connector; keep it.
identityToken =

## Takaro connector WebSocket URL; keep the default.
takaroWsUrl = wss://connect.takaro.io/

## Connector log level.
logLevel = Information

## Forward connector log events to Takaro.
enableLogEvents = true

## Semicolon-separated exact console commands allowed for executeConsoleCommand.
commandAllowlistExact = help

## Semicolon-separated console command prefixes allowed for executeConsoleCommand.
commandAllowlistPrefixes =

## Name shown in game chat for Takaro messages (unless Takaro sends its own sender name).
chatSenderName = Takaro
```

Leave `identityToken` empty; the connector fills it in. Keep it once set.

| Optional setting | What it does |
|---|---|
| `serverName` | Server name in Takaro. |
| `chatSenderName` | Sender name on messages from Takaro, used when none is set for the server in Takaro (default `Takaro`). |
| `commandAllowlistExact`, `commandAllowlistPrefixes` | Console commands Takaro may run, separated by `;` (restart to apply). |

### 4. Check that it works

The console (or `BepInEx/LogOutput.log`) shows
`Takaro Valheim identified as gameServerId=<id>` and the server shows as online in Takaro. Without
a token, a banner names the file to fix; a wrong token shows `Takaro rejected this server:`.

**Upgrading:** stop the server, replace `BepInEx/plugins/TakaroValheim/` and delete
`BepInEx/cache/chainloader_typeloader.dat`. From 4.2.x or older, see [UPGRADING.md](UPGRADING.md).

## What works, what doesn't

Checked on Valheim 1.0.20 (Steam build 25909431), 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | |
| Player list | ✅ | Name and Steam id. |
| Player location | ✅ | |
| Teleport | ✅ | |
| Player inventory | ⚠️ | Only for players with the optional inventory mod. |
| Item list | ✅ | |
| Entity list | ✅ | |
| Locations | ⚠️ | Stones, altars and traders, after Resync Locations. Not checked yet. |
| World map | ⚠️ | Drawn from the seed, about 4 minutes after the first start. Not checked yet. |
| Send a message | ✅ | Broadcasts and whispers. |
| Console commands | ⚠️ | Allowlisted commands only (default `help`). |
| Kick | ✅ | |
| Ban and unban | ⚠️ | The reason is discarded. |
| Ban list | ✅ | |
| Give an item | ✅ | Dropped at the player's feet. |
| Shop | ✅ | Delivered at the buyer's feet. |
| Shut down the server | ✅ | |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | |
| Entity killed | ✅ | |
| Server log lines | ⚠️ | Takaro does not keep them. |
| Discord chat bridge | ⚠️ | Discord to game works; game to Discord not checked. |
| Modules | ✅ | |
| Game name, version and icon | ✅ | The icon can take about two hours to appear. |
| Restarts and outages | ✅ | Events from an outage are sent afterwards. |
| Single player lookup | ❌ | Takaro uses the player list. |

## Known issues

- Players see a "Takaro" entry in the in-game player list; it is how server chat works.
- Given and shop items drop at the player's feet; anyone nearby could grab them first.
- A shop order placed while the server is disconnected is not delivered, but the coins are taken.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
