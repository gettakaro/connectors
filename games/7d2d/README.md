# Takaro 7D2D Connector

The Takaro connector for 7 Days to Die (version **0.6.0**) is a server-side mod that connects a <!-- x-release-please-version -->
7 Days to Die dedicated server to Takaro. Players do not install anything.

It is built once per exact server build, and each zip names the build it is for:

| Server build | Steam build (app 294420, branch `public`) | Zip | Status |
|---|---|---|---|
| **V 3.4.0 b9** | 26237934 | `takaro-7d2d-mod-linux-3.4.0.b9-<version>.zip` | candidate, proven live on 2026-10-29 |
| **V 3.4.0 b6** | 26167630 | `takaro-7d2d-mod-linux-3.4.0.b6-<version>.zip` | candidate, proven live on 2026-10-26 |
| **V 3.3.0 b18** | 25661908 | `takaro-7d2d-mod-linux-3.3.0.b18-<version>.zip` | maintained (default), proven live on 2026-10-06 |
| **V 3.2.0 b10** | 24994542 | `takaro-7d2d-mod-linux-3.2.0.b10-<version>.zip` | candidate, proven live on 2026-10-01 |

Use the zip that matches your server: V 3.3.0 changed the game's player-data and item APIs, so
the V 3.2.0 build cannot read inventories or set weapon mod quality on V 3.3.0 or later. The connector may
load on another build, but nothing here says it works there.

Moving an existing game server from Takaro's built-in 7 Days to Die integration? Install as below,
then follow [MIGRATION.md](MIGRATION.md).

## Install

Download the latest release: https://takaro.io/connectors/7d2d

### 1. Before you start

You need:

- A **7 Days to Die dedicated server** (Linux or Windows) that you can stop, start and copy files to.
- Access to the server's **`Mods/` folder** (create it if it does not exist).
- Outbound internet access from the server to `wss://connect.takaro.io/`. The connector dials out,
  so there is no port to open.
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

### 2. Download the connector

From the latest `7d2d-vX.Y.Z` release, on https://takaro.io/connectors/7d2d or on the releases page
(https://github.com/gettakaro/connectors/releases), download the zip for your server build:

- V 3.4.0 b9: **`takaro-7d2d-mod-linux-3.4.0.b9-<version>.zip`**
- V 3.4.0 b6: **`takaro-7d2d-mod-linux-3.4.0.b6-<version>.zip`**
- V 3.3.0 b18: **`takaro-7d2d-mod-linux-3.3.0.b18-<version>.zip`**
- V 3.2.0 b10: **`takaro-7d2d-mod-linux-3.2.0.b10-<version>.zip`**

The console shows your build on startup (`Version: V 3.3.0 (b18)`). `SHA256SUMS` in the same
release lets you check the download (`sha256sum -c SHA256SUMS`). `takaro-7d2d-mod.zip` is the same
bytes as the V 3.2.0 b10 zip, kept so old links keep working; do not use it on V 3.3.0. Do not use
the `7d2d-dev` pre-release; that is an untested rolling build.

The zip contains a single folder, `Takaro/`. That whole folder is the connector.

### 3. Copy it into place

Stop the server, then unzip so that the `Takaro` folder ends up directly inside `Mods/`:

```
<server>/Mods/Takaro/
    ModInfo.xml
    Takaro.dll
```

Harmony comes with the game (`Mods/0_TFP_Harmony`), so the mod ships nothing else.

Upgrading from a release whose zip also held `0Harmony.dll`, `MonoMod.*.dll`, `LiteDB.dll` and
other DLLs: delete the old `Mods/Takaro` folder before you unzip, so none of those copies stay
behind. Your settings are not in that folder and are kept.

Examples:

- Linux: `/home/steam/7dtd/Mods/Takaro/`
- Windows: `C:\7DaysToDieServer\Mods\Takaro\`

### 4. Configure

The connector keeps its settings in **`<server>/Takaro/Config.xml`**: a `Takaro` folder next to
`Mods/`, in the folder the server starts in (for example `/home/steam/7dtd/Takaro/Config.xml` or
`C:\7DaysToDieServer\Takaro\Config.xml`). The zip does not contain this file.

Either start the server once (the connector creates the file, with a new `IdentityToken`), or create
it yourself before the first start with this content:

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

Paste your Takaro registration token between the `RegistrationToken` tags and save. Leave `<Url>`
as it is, and leave `IdentityToken` empty; the connector fills it in and uses it to identify this
server in Takaro (unless you are migrating an existing game server, see [MIGRATION.md](MIGRATION.md)).

Saving is enough: the connector checks the file every few seconds and connects as soon as it has a
token. No restart needed. Emptying the token disconnects the server.

**Server chat name (optional).** Messages from Takaro (module replies, the Discord bridge,
broadcasts) show **Server** as the sender. To show a name of your own, set the server's chat name
in Takaro. On a Takaro version that does not pass that name to the connector yet, put it in
`<ServerChatName>` instead; a file from an older connector version has no `<Chat>` section, so add
it as above. The chat name set in Takaro wins over `ServerChatName`. The change applies within a
few seconds, without a restart. Players then see `Your Name: message` in global chat and
`From Your Name: message` in a whisper.

**Map (optional).** For the world map in Takaro, turn on the game's own map rendering in
`serverconfig.xml` and restart the server:

```xml
<property name="EnableMapRendering" value="true"/>
```

The server then draws map tiles while players explore, and the mod offers them to Takaro. The
web dashboard (`WebDashboardEnabled`) is not needed. With rendering off and no tiles saved, the mod
does not declare a map and Takaro shows none.

### 5. Check that it worked

In the server console / server log:

```
[MODS] Loaded Mod: Takaro (<version>)
```

In the connector's own log at `<server>/Takaro/logs/<M-D-YYYY>.log` (for example
`Takaro/logs/10-22-2026.log`):

```
Mod initialized successfully
Takaro accepted identify: game server <id>, protocol version 1
WebSocket connection confirmed (identify accepted); releasing 0 buffered outbound message(s)
```

Against an older Takaro the same line says `protocol version 0`; the connector works with both.

And in Takaro, the game server shows as **online**.

If no token is set yet, the server log shows a banner instead:

```
*************************************************************************
  RegistrationToken not set, the server is not connected to Takaro.
  Paste the registration token from Takaro into /home/steam/7dtd/Takaro/Config.xml
  and save it. The mod connects within a few seconds, no restart needed.
*************************************************************************
```

If the token is wrong, the banner says `Takaro rejected identify (error): Invalid
registrationToken provided.` Fix the token in `<server>/Takaro/Config.xml` and save; the connector
reconnects within a few seconds.

### 6. Upgrading

**Stop the server first.** Delete `<server>/Mods/Takaro/` and unzip the new version in its place,
then start the server again. Replace only `Mods/Takaro/`; your config at `<server>/Takaro/Config.xml`
is never touched, so the token and the server's identity in Takaro stay as they are. Never swap
`Takaro.dll` under a running server; it can crash the server.

## What works, what doesn't

Verified end to end on **2026-10-01** against a real dedicated server (game build **V 3.2.0 b10**,
Steam build 24994542, connector 0.2.1) with a real game client connected.
✅ = works, ⚠️ = works with a caveat, ❌ = does not work.

Re-proven on **2026-10-06** on **V 3.3.0 b18** (Steam build 25661908) with a real V 3.3.0 (b18)
game client: the server connects to Takaro on startup, and player join, chat, death, zombie kill
and disconnect all reach Takaro. Player list, inventory (items given through Takaro show up with
the right quality and amount), give item, teleport, kick, timed ban and unban (the client is
refused, then joins again), server messages, console commands, `@ping`, shutdown and the Discord
chat bridge from game to Discord all work. On 2026-10-08 a Discord post reached the V 3.3.0 b18
server chat with no client connected. The rows not listed here are from the V 3.2.0 b10 run.

Re-proven on **2026-10-29** on **V 3.4.0 b9** (Steam build 26237934) with a real V 3.4.0 (b9) game
client and no source change: player join, chat, `@ping`, death, zombie kill and disconnect reach
Takaro; player list, give item, inventory, teleport and location, kick, timed ban (the client is
refused) and unban (the client joins again), server messages, console commands, shutdown and the
Discord chat bridge in both directions work.

Re-proven on **2026-10-26** on **V 3.4.0 b6** (Steam build 26167630) with a real V 3.4.0 (b6) game
client and no source change: player join, chat, `@ping`, death, zombie kill and disconnect reach
Takaro; player list, give item, inventory, teleport and location, kick, timed ban (the client is
refused) and unban (the client joins again), server messages, console commands, shutdown and the
Discord chat bridge in both directions work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | Reconnects by itself after outages and server restarts, within about a minute of the network coming back. Events from during the outage are delivered afterwards, none lost. A wrong registration token is logged as an error. |
| Server restart / reconnect | ✅ | The connector comes back on its own after a server restart, including with a player online. |
| Player list | ✅ | Name, Steam id (or Xbox id), Epic (EOS) id, platform id, IP and ping. This is how Takaro loads players for 7D2D. |
| Moving from Takaro's built-in 7 Days to Die integration | ⚠️ | See [MIGRATION.md](MIGRATION.md). Players already known to Takaro from the built-in integration are recognised by their Steam/Epic id and keep their profile. If an older version of this connector already created a second profile for a player (one with an empty Steam ID), delete that second profile in Takaro once; until then Takaro ignores that player's events. |
| Single player lookup | ⚠️ | The data is correct, but Takaro never asks for one player at a time on this game — it uses the player list instead. |
| Player location | ✅ | Polled about every 30 s; matches the server's own `lp` output. |
| Player inventory | ✅ | Matches what the player is carrying in game. |
| Item catalogue | ✅ | Items with player-facing names; internal game entries that have no name are not sent (they can still be given by code). Takaro keeps rows it already has, so a server set up with an older version still lists those entries. |
| Entity catalogue | ✅ | 175 entities synced, with their in-game names. |
| Chat messages from players | ✅ | Real player chat reaches Takaro with the player attached. |
| Broadcast a message | ✅ | Shown to everyone in the server chat. |
| Whisper a player | ⚠️ | The message reaches the intended player. Only one client was connected, so "nobody else sees it" has not been confirmed. |
| Give an item | ✅ | Goes straight into the player's inventory, with the right amount and quality, and stacks onto what they already carry. If the inventory is full, what does not fit lands at the player's feet. |
| Teleport a player | ⚠️ | Works; the height (Y) is snapped to the ground, so the player lands on solid ground rather than at the exact Y you asked for. |
| Run a console command | ✅ | Output and success/failure are returned, including the error text for unknown commands. |
| Kick a player | ✅ | The player is dropped from the server with the reason shown. |
| Ban a player (timed and permanent) | ⚠️ | The ban lands on the game server and the player is blocked. Takaro does not emit its own ban events for it, so the ban is only visible on the game side and in the ban list. |
| Unban a player | ⚠️ | Clears the ban on the game server; same missing Takaro ban events as above. |
| Ban list | ✅ | Matches the server's own ban list. |
| Shut the server down | ✅ | The server stops cleanly on request. |
| Player joined event | ✅ | Arrives in Takaro about 1 s after the join. |
| Player left event | ✅ | Arrives in Takaro shortly after the player disconnects. |
| Player chat event | ✅ | See "Chat messages from players". |
| Player death event | ✅ | Includes the position where the player died. |
| Entity kill event | ✅ | Proven with real kills; reports the creature's class name, as Takaro's built-in 7 Days to Die integration does (for example "zombieBoe"), and the weapon's in-game name ("Steel Club", "Hunting Knife"). |
| Log events | ⚠️ | The connector sends them, but Takaro does not store server log lines as events, so they cannot be searched or used in modules. |
| World map | ✅ | Needs `EnableMapRendering=true` (see "Configure"). Takaro shows the tiles the server has rendered, north up, with players at their position; areas nobody has explored yet stay empty. |
| Locations / points of interest | ⚠️ | The connector collects them (368 found) and declares `locations.list`, which Takaro can now request on protocol version 1. Not yet proven live. |
| Discord chat bridge | ⚠️ | Both directions work: game chat reaches Discord, and a message posted in Discord shows up in game. Takaro also forwards the game's copy of a Discord message back to Discord once (Takaro-side echo). |
| Shop & economy | ✅ | Buying in game (`/shop`), ordering through the Takaro API, currency grants and balance checks all work. Purchases go straight into the player's inventory. |

### PrismaCore-compatible console commands

Verified end to end on **2026-10-22** against a real dedicated server (**V 3.3.0 b18**) with a
real game client, through Takaro's console command action. The commands keep PrismaCore's names
and output, so tools written for PrismaCore (StaffCommands among them) work unchanged. Running
PrismaCore next to this mod is not supported; the mod logs one warning at game start when it finds it.

| What | | Notes |
|---|---|---|
| `resetlevel` / `rl <player> <level>` | ✅ | The level changes in the player's character window at once, without a relog, and is still there after a death and a relog. |
| `resetskillpoints` / `rs <player> <count>` | ✅ | "Points available" in the skills window changes at once and is still there after a relog. |
| `isbloodmoon` | ✅ | Prints `false` before dusk on the blood moon day, `true` during a forced blood moon, `false` again the morning after. |
| Argument and offline-player errors | ✅ | Every line of the PrismaCore output contract, verbatim; a player who is not online gets the offline line, an unknown id gets the game's own `... is not a valid entity id, player name or user id.` |
| Player ids | ✅ | Names, `EOS_` and `Steam_` ids proven live; entity ids, `XBL_` and `PSN_` ids are covered by unit tests. |
| Player registry | ✅ | The mod remembers every player in `takaro.db` in the world's save folder, with their last position while online. Checked over 30 minutes of position sampling, 10 restarts in a row and 3 hard kills during writes: nothing durable was lost and the database opened cleanly every time. Players the game already knew before this version are picked up from its own player list. |
| PrismaCore warning | ✅ | Logged exactly once when a PrismaCore mod is loaded, never otherwise. |

### Known issues

- **Chat bridge echoes.** Takaro stores its own outgoing messages as chat, so the Discord bridge
  forwards the server's own message and loops a few rounds before settling. This is on the Takaro
  side — the connector only reports chat from real players.
- **Locations are not proven live.** The connector builds the catalogue and declares `locations.list`,
  but the request over protocol version 1 has only been checked against Takaro's expected shape.
- **The map shows explored areas only, and lags a little.** The server draws a tile when a player
  first loads that area. Takaro keeps tiles for up to 10 minutes, so a freshly explored area can
  take that long to appear or fill in.
- **Events from an outage arrive late, stamped with the delivery time.** Takaro records the moment
  it received an event, so after an outage the events from it show the reconnect time, not when they
  happened.
- **An event can arrive twice after an outage on older Takaro.** The connector resends anything Takaro had
  not yet confirmed when the connection died. On protocol version 1 Takaro numbers and drops the
  duplicate; on an older Takaro (protocol version 0) it shows up twice.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
