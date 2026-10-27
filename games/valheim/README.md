# Takaro Valheim Connector

A BepInEx plugin that runs inside a Valheim dedicated server and connects it to Takaro. Players do
not install anything: they join with plain Valheim. Only reading a player's inventory needs an
optional mod on that player's game ([step 7](#7-optional-inventory-mod-for-players)).

## Install

### 1. Before you start

- A Linux **Valheim dedicated server** (Windows servers are not tested) with **BepInExPack Valheim
  5.4.2351** (`denikson/BepInExPack_Valheim` on Thunderstore). Start it once so
  `BepInEx/plugins/` exists.
- A Takaro game server of type **Generic** and its **registration token**.
- Outbound access to `wss://connect.takaro.io/`. No ports need to be opened.

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `linux-1.0.20` | 1.0.20 | linux | — | None | candidate | contract |
<!-- takaro-maint:targets:end -->

### 2. Download

Download **`takaro-valheim-plugin.zip`** from <https://takaro.io/connectors/valheim> or the latest
`valheim-v4.x.x` release on <https://github.com/gettakaro/connectors/releases>. Do not use the
`valheim-dev` pre-release.

### 3. Copy it into place

Stop the server. Unzip so the `TakaroValheim` folder lands in `BepInEx/plugins/`:

```
<server>/BepInEx/plugins/TakaroValheim/TakaroValheim.dll   (plus the other files from the zip)
```

### 4. Set the registration token

Start the server. The connector creates `BepInEx/config/com.takaro.valheim.cfg`. Paste your token
after `registrationToken =` and save: it connects within a few seconds, no restart needed. You can
also write the file yourself before the first start. With its defaults it looks like this:

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

### 5. Check that it worked

The server log (`BepInEx/LogOutput.log`) shows
`Takaro Valheim identified as gameServerId=<id> (protocol version 1).` and the server is
**online** in Takaro. A banner of `*` lines means the token is missing, wrong (`Takaro rejected
this server:`) or the name is taken (`Takaro refused the server name`); fix the file and save.

### 6. Upgrading

Stop the server, delete `BepInEx/plugins/TakaroValheim/`, unzip the new version in its place and
delete `BepInEx/cache/chainloader_typeloader.dat`. Start the server. Your
`com.takaro.valheim.cfg` is kept.

**Updating from 4.2.x:** do not delete the folder. Unzip a release newer than 4.3.0 over it,
delete the cache file and start. The connector moves your `takaro.cfg` settings into
`com.takaro.valheim.cfg` by itself. Older versions: see [UPGRADING.md](UPGRADING.md).

### 7. Optional: inventory mod for players

Only needed for Takaro to see a player's inventory; players without it play normally.

1. On the player's PC, install BepInExPack Valheim 5.4.2351 into the Valheim game folder.
2. Unzip **`takaro-valheim-inventory-companion.zip`** from the same release into that game's
   `BepInEx/plugins/`.
3. Start Valheim and join the server.

## What works, what doesn't

Tested on Valheim 1.0.20 on 2026-10-26.

| Feature | Status | Note |
|---|---|---|
| Connection, restart, reconnect | ✅ | Reconnects on its own after a server restart. |
| Game name, build and icon | ✅ | Icon can take up to two hours on a fresh install. |
| Player list | ✅ | Name and Steam ID. |
| Single player lookup | ❌ | Takaro uses the player list instead. |
| Player location | ✅ | |
| Player inventory | ⚠️ | Only for players with the optional inventory mod. |
| Give an item | ✅ | Dropped at the player's feet. |
| Item and entity catalogue | ✅ | English in-game names. |
| Locations | ✅ | Altars, traders, Sacrificial Stones. |
| World map | ✅ | Ready about 4 minutes after first start. |
| Run a console command | ⚠️ | Allowlisted commands only; default is just `help`. |
| Broadcast and whisper | ✅ | Whisper not tested with two players. |
| Teleport a player | ✅ | |
| Kick, ban, unban, ban list | ⚠️ | Ban reason is discarded by Valheim. |
| Shut the server down | ✅ | |
| Join, leave, chat, death events | ✅ | |
| Entity kill event | ✅ | Creature and weapon included. |
| Log events | ⚠️ | Sent, but Takaro does not store them. |
| Modules: commands, hooks, cronjobs, teleports | ✅ | |
| Shop and economy | ✅ | Items drop at the buyer's feet. |
| Discord: game chat to Discord | ⚠️ | Takaro posts it; not checked in the Discord app. |
| Discord: Discord to game chat | ✅ | |
| Config change without a restart | ✅ | |
| Takaro outage | ✅ | Events are delivered after reconnecting. |

## Known issues

- Players see a "Takaro" entry in the in-game player list; server chat needs it.
- Items given or bought drop at the player's feet; anyone nearby can grab them.
- A shop order placed while the server is disconnected is not delivered, but is still paid.
- Some Valheim console commands, such as `save`, fail on a dedicated server.
