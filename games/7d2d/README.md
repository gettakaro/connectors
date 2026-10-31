# Takaro 7D2D Connector

The Takaro connector for 7 Days to Die (version **0.7.0**) is a server-side mod for Linux and <!-- x-release-please-version -->
Windows dedicated servers. Players do not install anything.

## Install

You need: a 7 Days to Die dedicated server on one of the builds below, and a Takaro game server of
type **Generic** with its registration token. Moving from Takaro's built-in 7 Days to Die
integration? Install as below, then follow [MIGRATION.md](MIGRATION.md).

### 1. Download

From https://takaro.io/connectors/7d2d, download the zip for your server build (the console shows
it on startup, for example `Version: V 3.3.0 (b18)`):

| Server build | Zip |
|---|---|
| V 3.4.0 b9 | `takaro-7d2d-mod-linux-3.4.0.b9-<version>.zip` |
| V 3.4.0 b6 | `takaro-7d2d-mod-linux-3.4.0.b6-<version>.zip` |
| V 3.3.0 b18 | `takaro-7d2d-mod-linux-3.3.0.b18-<version>.zip` |
| V 3.2.0 b10 | `takaro-7d2d-mod-linux-3.2.0.b10-<version>.zip` |

The same zip works on Linux and Windows. Do not use the `7d2d-dev` pre-release.

### 2. Install

Stop the server and unzip so the `Takaro` folder sits directly inside `Mods/`:

```text
<server>/Mods/Takaro/ModInfo.xml
<server>/Mods/Takaro/Takaro.dll
```

### 3. Configure

The config is `<server>/Takaro/Config.xml` (a `Takaro` folder next to `Mods/`, not inside it). The
mod creates it on first start; you can also create it yourself:

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
-->
<Takaro>
  <WebSocket>
    <Url>wss://connect.takaro.io/</Url>
    <RegistrationToken></RegistrationToken>
    <IdentityToken></IdentityToken>
    <Enabled>true</Enabled>
    <ReconnectIntervalSeconds>30</ReconnectIntervalSeconds>
  </WebSocket>
  <Chat>
    <ServerChatName></ServerChatName>
  </Chat>
</Takaro>
```

Paste your registration token between the `RegistrationToken` tags and save. Leave
`IdentityToken` empty; the mod fills it in. The mod picks up changes within a few seconds, no
restart needed.

| Optional setting | What it does |
|---|---|
| `<ServerChatName>` | Sender name on messages from Takaro, used when none is set for the server in Takaro. |
| `EnableMapRendering` in `serverconfig.xml` | Set to `true` and restart to get the world map in Takaro. |

### 4. Check that it works

The server log shows `[MODS] Loaded Mod: Takaro (<version>)`, the mod's log
`<server>/Takaro/logs/<M-D-YYYY>.log` shows `Takaro accepted identify: game server <id>`, and the
server shows as online in Takaro. Without a token the server log shows a banner naming the file to
fix; a wrong token shows `Takaro rejected identify (error): Invalid registrationToken provided.`

**Upgrading:** stop the server, delete `<server>/Mods/Takaro/` and unzip the new version. Your
`Takaro/Config.xml` stays.

## What works, what doesn't

Checked on V 3.4.0 b9 (Steam build 26237934), 2026-10-29. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects after an outage; missed events are sent afterwards. |
| Player list | ✅ | Steam or Xbox id, Epic id, IP and ping. |
| Player location | ✅ | Updated about every 30 seconds. |
| Teleport | ⚠️ | Lands on the ground, not at the exact height. |
| Player inventory | ✅ | |
| Item list | ✅ | |
| Entity list | ✅ | |
| Locations | ⚠️ | Points of interest. Not checked yet. |
| World map | ✅ | Needs `EnableMapRendering`; explored areas only. |
| Send a message | ✅ | Broadcasts and whispers. |
| Console commands | ✅ | |
| Kick | ✅ | |
| Ban and unban | ⚠️ | Works, but Takaro records no ban event. |
| Ban list | ✅ | |
| Give an item | ✅ | Extra items drop at the player's feet. |
| Shop | ✅ | |
| Shut down the server | ✅ | |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | |
| Entity killed | ✅ | Shows the creature's class name, for example `zombieBoe`. |
| Server log lines | ⚠️ | Takaro does not keep them. |
| Discord chat bridge | ⚠️ | Both ways, but one message can echo back to Discord. |
| Modules | ✅ | |
| Moving from the built-in integration | ⚠️ | Players keep their profile; see [MIGRATION.md](MIGRATION.md). |
| PrismaCore commands | ✅ | `resetlevel`, `resetskillpoints` and `isbloodmoon` work like PrismaCore's. |

## Known issues

- Do not run PrismaCore next to this mod.
- Newly explored map areas can take up to 10 minutes to appear.
- A player with a second profile with an empty Steam id, made by an older version, has their
  events ignored until that profile is deleted in Takaro.
- Never replace `Takaro.dll` while the server runs; it can crash the server.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
