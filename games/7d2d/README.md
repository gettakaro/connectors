# Takaro 7D2D Connector

A server-side mod that connects a 7 Days to Die dedicated server (Linux or Windows) to Takaro. It
is built for exact server builds. Players do not install anything.

## Install

### 1. Before you start

- A 7 Days to Die dedicated server (Linux or Windows) that you can stop, start and copy files to,
  on build **V 3.4.0 b10**, **V 3.4.0 b9**, **V 3.4.0 b6**, **V 3.3.0 b18** or **V 3.2.0 b10**.
- Access to the server's `Mods/` folder (create it if it does not exist).
- Outbound internet access from the server to `wss://connect.takaro.io/`. The connector dials out,
  so there is no port to open.
- A Takaro game server of type **Generic** and its **registration token**.
- Moving an existing game server from Takaro's built-in 7 Days to Die integration? Install as
  below, then follow [MIGRATION.md](MIGRATION.md).

### 2. Download

Download the latest release from https://takaro.io/connectors/7d2d (or from the releases page,
https://github.com/gettakaro/connectors/releases). Take the zip for your server build:

| Server build | Steam build (app 294420, branch `public`) | Zip |
|---|---|---|
| V 3.4.0 b10 | 26310339 | `takaro-7d2d-mod-3.4.0.b10-<version>.zip` |
| V 3.4.0 b9 | 26237934 | `takaro-7d2d-mod-3.4.0.b9-<version>.zip` |
| V 3.4.0 b6 | 26167630 | `takaro-7d2d-mod-3.4.0.b6-<version>.zip` |
| V 3.3.0 b18 (default) | 25661908 | `takaro-7d2d-mod-3.3.0.b18-<version>.zip` |
| V 3.2.0 b10 | 24994542 | `takaro-7d2d-mod-3.2.0.b10-<version>.zip` |

The console shows your build on startup (`Version: V 3.3.0 (b18)`). Use the zip that matches.
The same zip works on Linux and Windows servers. V 3.3.0 changed the game's player-data and item
APIs, so a V 3.2.0 zip on V 3.3.0 or later (or the other way round) cannot read inventories or give
items (`Field not found: int .ItemValue.type`). The mod checks this on startup: on the wrong build it
logs an error naming the zip you need, and
`getPlayerInventory` fails with that message instead of returning an empty inventory. On another
build of the same game version it logs a warning, because it is not checked there. Releases up to
0.7.x also had a `takaro-7d2d-mod.zip`: that was the V 3.2.0 b10 build, so do not use it on V 3.3.0
or later. Do not use the `7d2d-dev` pre-release. Check the download with `SHA256SUMS` from the same
release (`sha256sum -c SHA256SUMS`).

### 3. Install

Stop the server, then unzip so that the `Takaro` folder ends up directly inside `Mods/`:

```
<server>/Mods/Takaro/
    ModInfo.xml
    Takaro.dll
```

Harmony comes with the game (`Mods/0_TFP_Harmony`), so the mod ships nothing else.

- Linux: `/home/steam/7dtd/Mods/Takaro/`
- Windows: `C:\7DaysToDieServer\Mods\Takaro\`

### 4. Configure

The settings are in `<server>/Takaro/Config.xml`: a `Takaro` folder next to `Mods/`, in the folder
the server starts in (for example `/home/steam/7dtd/Takaro/Config.xml` or
`C:\7DaysToDieServer\Takaro\Config.xml`). The zip does not contain it. Start the server once and the
connector creates it, or create it yourself before the first start:

```xml
<?xml version="1.0" encoding="utf-8"?>
<!--
  Takaro connection settings.

  Paste the registration token Takaro shows when you add a Generic game server
  into RegistrationToken and save. The mod picks the change up within a few
  seconds, no restart needed.

  IdentityToken identifies this server in Takaro; the mod fills it in when it
  is empty. Keep it if you move or reinstall the server.

  ServerChatName is the sender players see on messages from Takaro, instead
  of "Server". The chat name set in Takaro wins over it; leave both empty
  for "Server".

  ReportCharacters tells Takaro about each life on a Permanent Death server.
  Turn it on only when your Takaro supports Characters, then restart the server.
-->
<Takaro>
  <WebSocket>
    <Url>wss://connect.takaro.io/</Url>
    <RegistrationToken></RegistrationToken>
    <IdentityToken></IdentityToken>
    <Enabled>true</Enabled>
    <ReconnectIntervalSeconds>2</ReconnectIntervalSeconds>
  </WebSocket>
  <Chat>
    <ServerChatName></ServerChatName>
  </Chat>
  <Players>
    <ReportCharacters>false</ReportCharacters>
  </Players>
</Takaro>
```

Paste your registration token between the `RegistrationToken` tags and save. Leave `<Url>` as it
is and leave `IdentityToken` empty; the connector fills it in (unless you are migrating, see
[MIGRATION.md](MIGRATION.md)). No restart is needed: the connector checks the file every few
seconds and connects as soon as it has a token. Emptying the token disconnects the server.

**Reconnecting.** When a working connection drops, the connector reconnects within about a
second. Only when attempts keep failing does it wait longer between them: from
`ReconnectIntervalSeconds` (default 2), doubling each time, at most 60 seconds, with a random
spread. A file written by an older connector says 30; that still works (the backoff then starts
at 30 seconds), and you can change it to 2.

**Server chat name (optional).** Messages from Takaro show **Server** as the sender. To show your
own name, set the server's chat name in Takaro. If your Takaro does not pass that name to the
connector yet, put it in `<ServerChatName>` (a file from an older connector version has no `<Chat>`
section; add it as above). The name set in Takaro wins. It applies within a few seconds; players
see `Your Name: message` in global chat and `From Your Name: message` in a whisper.

**Characters (optional).** On a server with the Permanent Death penalty, the connector can report
each life as a separate Character. Set `<ReportCharacters>` to `true` and restart the server, but
only if your Takaro supports Characters: older versions refuse player events that carry one.

**Map (optional).** For the world map in Takaro, turn on the game's map rendering in
`serverconfig.xml` and restart the server:

```xml
<property name="EnableMapRendering" value="true"/>
```

The web dashboard (`WebDashboardEnabled`) is not needed. With rendering off and no tiles saved,
Takaro shows no map.

### 5. Check that it works

The server log shows:

```
[MODS] Loaded Mod: Takaro (<version>)
```

The connector's own log, `<server>/Takaro/logs/<M-D-YYYY>.log` (for example
`Takaro/logs/10-22-2026.log`), shows:

```
Mod initialized successfully
Takaro accepted identify: game server <id>, protocol version 1
WebSocket connection confirmed (identify accepted); releasing 0 buffered outbound message(s)
```

(On an older Takaro the second line says `protocol version 0`; that is fine.) The game server shows
as **online** in Takaro.

Without a token, the server log shows a banner:

```
*************************************************************************
  RegistrationToken not set, the server is not connected to Takaro.
  Paste the registration token from Takaro into /home/steam/7dtd/Takaro/Config.xml
  and save it. The mod connects within a few seconds, no restart needed.
*************************************************************************
```

With a wrong token it says `Takaro rejected identify (error): Invalid registrationToken provided.`
Fix the token in `<server>/Takaro/Config.xml` and save; the connector reconnects within a few
seconds.

### 6. Upgrade

Stop the server, delete `<server>/Mods/Takaro/`, unzip the new version in its place and start the
server. Deleting the folder also removes the extra DLLs (`0Harmony.dll`, `MonoMod.*.dll`,
`LiteDB.dll`, …) that older releases shipped. `<server>/Takaro/Config.xml` and the player registry
`takaro.db` in the world's save folder are never touched, so the token and the server's identity
stay. Never swap `Takaro.dll` under a running server; it can crash the server.

Upgrading from 0.9.0 or older: those versions also sent the game's Epic id for Steam and Xbox
players, newer ones do not. Players Takaro already knows keep their profile, so this change needs
no action.

## What works, what doesn't

Checked on V 3.4.0 b10 (Steam build 26310339), 2026-11-02. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects within about a second when a working connection drops, and by itself within a minute after a longer outage or a server restart; events from the gap are delivered afterwards. A link that goes silent is noticed within 45 seconds. |
| Player list | ✅ | Name, Steam id (or Xbox id), platform id, IP and ping. The Epic (EOS) id is sent only for players who join through the Epic store: Takaro keeps one Epic id per player and the game's Epic id differs per game. |
| Player location | ✅ | Updated about every 30 seconds. |
| Teleport | ⚠️ | The height is snapped to the ground, so the player lands on solid ground, not at the exact height asked for. |
| Player inventory | ✅ | |
| Player levels and skills | ⚠️ | Character level with experience, plus every attribute, perk and crafting skill as its own track. The game client owns these values and reports them to the server, so Takaro marks them client-reported. |
| Item list | ✅ | Player-facing names; internal entries without a name are left out but can still be given by code. A server set up with an older version keeps those old rows. |
| Item icons | ⚠️ | Real icons from the server's files and installed mods reach Takaro with no setup. Items the game tints when it draws them keep Takaro's own icon. |
| Entity list | ✅ | With in-game names. |
| Locations | ⚠️ | The connector collects the points of interest; not checked in Takaro yet. |
| World map | ✅ | Needs `EnableMapRendering` (see Configure). North up, players at their position; unexplored areas stay empty. |
| Send a message | ⚠️ | Broadcasts reach everyone. Whispers reach the player; whether others can see them is not checked. |
| Console commands | ✅ | Returns output and success or failure, including the error for unknown commands. |
| Kick | ✅ | The player sees the reason. |
| Ban and unban | ⚠️ | Timed and permanent bans work on the game server, but Takaro records no ban or unban event for them. |
| Ban list | ✅ | Matches the server's own ban list. |
| Give an item | ✅ | Right amount and quality, stacked onto what the player carries; what does not fit lands at the player's feet. |
| Shop | ✅ | Buying in game (`/shop`), orders, currency and balance; purchases go straight into the inventory. |
| Shut down the server | ✅ | |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | With the position. |
| Entity killed | ✅ | The creature's class name (for example `zombieBoe`) and the weapon's in-game name (`Steel Club`). |
| Server log lines | ⚠️ | Sent, but Takaro does not store them, so modules cannot use them. |
| Discord chat bridge | ⚠️ | Both directions work, but Takaro sends the game's copy of a Discord message back to Discord once. |
| Modules | ✅ | Chat commands such as `@ping`. |
| Moving from the built-in integration | ⚠️ | Known players keep their profile ([MIGRATION.md](MIGRATION.md)). A second profile with an empty Steam ID, made by an older connector version, must be deleted once; until then that player's events are ignored. |
| PrismaCore commands | ✅ | `resetlevel` / `rl <player> <level>`, `resetskillpoints` / `rs <player> <count>` and `isbloodmoon` with PrismaCore's names and output, so tools such as StaffCommands work unchanged. Changes show at once and survive death and relog. |
| PrismaCore player ids | ⚠️ | Names, entity ids, `EOS_` and `Steam_` ids work; `XBL_` and `PSN_` ids are not checked on a live server. Offline and unknown players get PrismaCore's own error lines. |

## Known issues

- Do not run PrismaCore next to this mod; the mod logs one warning at game start when it finds it.
- The Discord bridge can loop the server's own message a few rounds before settling, because
  Takaro stores its own outgoing messages as chat.
- The map shows explored areas only. Takaro keeps tiles for up to 10 minutes, so a newly explored
  area can take that long to appear.
- After an outage, delayed events show the reconnect time, not when they happened. On an older
  Takaro an event can also arrive twice.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
