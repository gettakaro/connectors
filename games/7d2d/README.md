# Takaro 7D2D Connector

The Takaro connector for 7 Days to Die (version **0.5.0**) is a server-side mod that connects a <!-- x-release-please-version -->
7 Days to Die dedicated server (Linux or Windows) to Takaro. Players do not install anything.

## Install

You need a 7 Days to Die dedicated server you can stop, start and copy files to, and a Takaro game
server of type **Generic** with its **registration token**. The connector dials out to
`wss://connect.takaro.io/`; there is no port to open.

Moving from Takaro's built-in 7 Days to Die integration? Install as below, then follow
[MIGRATION.md](MIGRATION.md).

### 1. Download

From https://takaro.io/connectors/7d2d (or the `7d2d-v<version>` release on
[GitHub](https://github.com/gettakaro/connectors/releases)), download the zip for your server build.
The console shows the build on startup (`Version: V 3.3.0 (b18)`).

| Server build | Zip |
|---|---|
| V 3.3.0 b18 (default) | `takaro-7d2d-mod-linux-3.3.0.b18-<version>.zip` |
| V 3.2.0 b10 | `takaro-7d2d-mod-linux-3.2.0.b10-<version>.zip` |

Do not use the V 3.2.0 zip on V 3.3.0, and do not use the untested `7d2d-dev` pre-release.

### 2. Copy it into place

Stop the server and unzip so the `Takaro` folder sits directly in `Mods/`:

```
<server>/Mods/Takaro/
    ModInfo.xml
    Takaro.dll
```

Harmony comes with the game, so nothing else is needed. If an older `Mods/Takaro` folder exists,
delete it first.

### 3. Set the registration token

Start the server once. The connector creates `<server>/Takaro/Config.xml` (a `Takaro` folder next
to `Mods/`, not inside it). You can also create it yourself before the first start:

```xml
<?xml version="1.0" encoding="utf-8"?>
<!--
  Takaro connection settings.

  Paste the registration token Takaro shows when you add a Generic game server
  into RegistrationToken and save. The mod picks the change up within a few
  seconds, no restart needed.

  IdentityToken identifies this server in Takaro; the mod fills it in when it
  is empty. Keep it if you move or reinstall the server.
-->
<Takaro>
  <WebSocket>
    <Url>wss://connect.takaro.io/</Url>
    <RegistrationToken></RegistrationToken>
    <IdentityToken></IdentityToken>
    <Enabled>true</Enabled>
    <ReconnectIntervalSeconds>30</ReconnectIntervalSeconds>
  </WebSocket>
</Takaro>
```

Paste the token between the `RegistrationToken` tags and save. The connector connects within a few
seconds; no restart needed. Leave `IdentityToken` empty; the connector fills it in.

**Map (optional).** Set `<property name="EnableMapRendering" value="true"/>` in `serverconfig.xml`
and restart. Takaro then shows the areas players have explored.

### 4. Check that it worked

The connector log `<server>/Takaro/logs/<M-D-YYYY>.log` shows:

```
Takaro accepted identify: game server <id>, protocol version 1
```

and Takaro shows the server as online. Without a token the server log shows a
`RegistrationToken not set` banner naming the file to edit; a wrong token shows
`Invalid registrationToken provided`.

### Updating

Stop the server, delete `<server>/Mods/Takaro/`, unzip the new version and start the server. Your
settings in `<server>/Takaro/Config.xml` are kept. Never replace `Takaro.dll` on a running server.

## What works, what doesn't

Tested on V 3.3.0 b18 on 2026-10-26. V 3.2.0 b10 was last tested on 2026-10-01.

| Feature | Status | Note |
|---|---|---|
| Connection and reconnect | ✅ | Reconnects by itself after outages and server restarts. |
| Player list | ✅ | Name, Steam/Xbox id, Epic id, IP and ping. |
| Single player lookup | ⚠️ | Correct, but Takaro uses the player list instead. |
| Player location | ✅ | |
| Player inventory | ✅ | |
| Item and entity catalogue | ✅ | In-game names. |
| Locations | ✅ | Points of interest of the world, with names. |
| World map | ✅ | Needs `EnableMapRendering`; only explored areas show. |
| Broadcast a message | ✅ | |
| Whisper a player | ⚠️ | Reaches the player; not tested with two players. |
| Give an item | ✅ | Right amount and quality; overflow drops at the player's feet. |
| Teleport a player | ⚠️ | Height is snapped to the ground. |
| Run a console command | ✅ | Output and errors are returned. |
| Kick a player | ✅ | |
| Ban, unban, ban list | ✅ | Timed and permanent; a banned player is refused. |
| Shut the server down | ✅ | |
| Player joined / left | ✅ | |
| Chat | ✅ | |
| Player death | ✅ | With position. Killer on PvP deaths not tested with two players. |
| Entity kill | ✅ | Creature class name (for example `zombieBoe`) and weapon name. |
| Log events | ⚠️ | Sent, but Takaro does not store them. |
| Modules, economy and shop | ✅ | Purchases go straight into the inventory. |
| Discord chat bridge | ✅ | Both ways; use the `chatBridgeNoEcho` module. |
| PrismaCore commands | ✅ | `resetlevel`/`rl`, `resetskillpoints`/`rs`, `isbloodmoon`, same output as PrismaCore. |

## Known issues

- Do not run PrismaCore next to this mod; the mod logs a warning when it finds it.
- New map areas can take up to 10 minutes to appear in Takaro.
- Events from a connection outage arrive late, stamped with the time they were delivered.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
