# Takaro Minecraft Connector

A server-side-only connector that connects a Minecraft dedicated server to Takaro. It ships as one
file for **Fabric**, **Paper** or **NeoForge**; players do not install anything and join with the
normal Minecraft client. Everything below was re-verified on all three platforms on
**2026-10-22** (see [What works, what doesn't](#what-works-what-doesnt)).

## Install

Download the latest release: https://takaro.io/connectors/minecraft

### 1. Before you start

You need:

- A **Minecraft dedicated server** you can stop, start and copy files to, running one of:
  - **Fabric** on **Minecraft 26.2** (or 26.1.2 / 26.3) with **Java 25** and the **Fabric API** mod
    installed (the connector requires Fabric Loader 0.19.5 or newer; 0.19.3 on 26.1.2).
  - **Paper** on **Minecraft 1.21.11** with **Java 21**.
  - **NeoForge** 21.11.45 or newer on **Minecraft 1.21.11** with **Java 21**.
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

The exact server builds this connector is maintained for:

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `fabric-26.1.2` | 26.1.2 | fabric | loader 0.19.3 / API 0.155.3+26.1.2 | 25 | maintained | protocol |
| `fabric-26.2` | 26.2 | fabric | loader 0.19.5 / API 0.160.0+26.2 | 25 | maintained | protocol |
| `fabric-26.3` | 26.3 | fabric | loader 0.19.5 / API 0.161.0+26.3 | 25 | maintained | protocol |
| `neoforge-1.21.11` | 1.21.11 | neoforge | loader 21.11.45 | 21 | maintained | protocol |
| `paper-1.21.11` | 1.21.11 | paper | loader 132 | 21 | maintained | protocol |
<!-- takaro-maint:targets:end -->

Players' clients must match the server's Minecraft version. A 26.2 client cannot join a 1.21.11
server, and a 1.21.11 client cannot join a 26.2 server.

### 2. Download the connector

From the downloads page above, take the **one** file that matches your server. Each file is named
after the server build it was made for:

| Your server | File to download |
|---|---|
| Fabric 26.2 | `takaro-minecraft-mod-fabric-26.2-<version>.jar` |
| Fabric 26.1.2 | `takaro-minecraft-mod-fabric-26.1.2-<version>.jar` |
| Fabric 26.3 | `takaro-minecraft-mod-fabric-26.3-<version>.jar` |
| Paper 1.21.11 | `takaro-minecraft-mod-paper-1.21.11-<version>.jar` |
| NeoForge 1.21.11 | `takaro-minecraft-mod-neoforge-1.21.11-<version>.jar` |

The same files are attached to every `minecraft-v<version>` release from 0.5.0 on, on
[GitHub](https://github.com/gettakaro/connectors/releases), with a `SHA256SUMS` file to check
them against. Do not use the `minecraft-dev` pre-release; that is an untested rolling build.
Older releases used other file names; see [UPGRADING.md](UPGRADING.md).

### 3. Copy it into place

Stop the server, then put the single jar into the right folder for your platform:

```
Fabric     <server>/mods/takaro-minecraft-mod-fabric-26.2-<version>.jar
NeoForge   <server>/mods/takaro-minecraft-mod-neoforge-1.21.11-<version>.jar
Paper      <server>/plugins/takaro-minecraft-mod-paper-1.21.11-<version>.jar
```

That is the whole install: one file, no extra libraries. On Fabric, keep the Fabric API jar in
`mods/` next to it.

### 4. Configure

Start the server. On its first start the connector writes its config file (you can also create
it yourself before the first start, with the content below):

| Your server | Config file |
|---|---|
| Fabric | `<server>/config/takaro.json` |
| NeoForge | `<server>/config/takaro.properties` |
| Paper | `<server>/plugins/TakaroMinecraft/config.yml` |

Open it, paste your Takaro **registration token** between the quotes (or after the `=`), and save.
Saving is enough: the connector checks the file every few seconds and connects as soon as the token
is saved, no restart needed. The connector fills in `identity_token` itself on the first start; it
is how Takaro knows this server, so keep it when you move or copy the server.

**Fabric** (`config/takaro.json`):

```json
{
  "websocket": {
    "url": "wss://connect.takaro.io/"
  },
  "authentication": {
    "identity_token": "",
    "registration_token": ""
  },
  "reconnect": {
    "enabled": true,
    "delay": 5000,
    "max_delay": 300000,
    "backoff_multiplier": 1.5
  },
  "settings": {
    "debug": false,
    "map": false
  }
}
```

**NeoForge** (`config/takaro.properties`):

```properties
takaro.websocket.url=wss://connect.takaro.io/
takaro.authentication.identity_token=
takaro.authentication.registration_token=
takaro.reconnect.enabled=true
takaro.reconnect.delay=5000
takaro.reconnect.max_delay=300000
takaro.reconnect.backoff_multiplier=1.5
takaro.debug=false
# World map in Takaro. Needs Takaro protocol v1, not yet on takaro.io.
takaro.map=false
```

**Paper** (`plugins/TakaroMinecraft/config.yml`):

```yaml
takaro:
  websocket:
    url: "wss://connect.takaro.io/"
  authentication:
    identity_token: ""
    registration_token: ""
  reconnect:
    enabled: true
    delay: 5000
    max_delay: 300000
    backoff_multiplier: 1.5
  debug: false
  # World map in Takaro. Needs Takaro protocol v1, not yet on takaro.io.
  map: false
```

`map` (in the Fabric file: `settings.map`; or `TAKARO_MAP=true`) turns on the world map in Takaro.
It needs Takaro protocol v1, which takaro.io does not have yet, so leave it `false` there. Changing
it reconnects to Takaro, no restart needed.

### 5. Check that it worked

On the first start, before a token is set, the server console (and `logs/latest.log`) shows a
banner naming the exact file to edit:

```
No identity token set; generated one for this server and saving it in /srv/mc/config/takaro.json
*************************************************************************
  registration_token not set, the server is not connected to Takaro.
  Paste the registration token from Takaro into /srv/mc/config/takaro.json
  and save it. The connector connects within a few seconds, no restart needed.
*************************************************************************
```

A few seconds after you save the token, these lines follow (Paper prefixes them with
`[TakaroMinecraft]`, NeoForge with `[Takaro/]`):

```
/srv/mc/config/takaro.json changed; reconnecting with the new settings
Takaro target-check: {"target":"fabric-26.2", ... "result":"ok","reasons":[]}
Connecting to Takaro at wss://connect.takaro.io/
Identified successfully, server ID: <your server id>
```

In Takaro, the game server now shows as **online**.

If the token is wrong, the banner starts with `Identify failed:` instead. Fix the token in the
config file and save; the connector reconnects within a few seconds.

If the jar does not match the server it runs on, the connector refuses to connect and says why:

```
Takaro refuses to connect: target fabric-26.2 does not match this server
(built for game version 26.2, this server runs 26.1.2).
```

Download the jar for your server's version instead (see [UPGRADING.md](UPGRADING.md) for the
override, which does not make an untested combination work).

### 6. Upgrading

**Stop the server first.** Delete the old Takaro jar from `mods/` (or `plugins/`) and drop the new
jar in its place, then start the server again. Leave the config file alone: it is not part of the
jar, so your token and identity survive the upgrade and the server keeps its place in Takaro.
Never swap the jar under a running server.

## What works, what doesn't

Verified end to end on **2026-10-22** against Takaro with a real Minecraft client connected, on:

- **Fabric**: Minecraft **26.2**, Fabric Loader 0.19.5, Fabric API 0.161.0+26.2, Java 25.
- **Paper**: Minecraft **1.21.11**, Paper build 132, Java 21.
- **NeoForge**: Minecraft **1.21.11**, NeoForge 21.11.45, Java 21.

The Fabric 26.1.2 and 26.3 files are built and checked the same way but were not played on in this
run. ✅ = works, ⚠️ = works with a caveat, ❌ = does not work, – = not tested on that platform.

| What | Fabric | Paper | NeoForge | Notes |
|---|---|---|---|---|
| Connection & heartbeat | ✅ | ✅ | ✅ | Connects within seconds of saving the token and stays up. |
| Server restart / reconnect | ✅ | ✅ | ✅ | Comes back on its own after a restart; Takaro marks it offline meanwhile. |
| Player list | ✅ | ✅ | ✅ | Name, id and ping of everyone online. |
| Single player lookup | ✅ | ✅ | ✅ | Name, platform id and online state match the game. |
| Player location | ✅ | ✅ | ✅ | Matches the game, rounded to whole blocks. |
| Player inventory | ✅ | ✅ | ✅ | Matches what the player carries; changes are reported as they happen. |
| Item catalogue | ✅ | ✅ | ✅ | About 1,500 items with their in-game names. |
| Entity catalogue | ✅ | ✅ | ✅ | Paper lists the 91 mob types; Fabric and NeoForge list every entity type (about 158, including things like Primed TNT). |
| Locations / points of interest | ❌ | ❌ | ❌ | Not implemented: always an empty list. Minecraft has no fixed list of named locations. |
| Chat messages from players | ✅ | ✅ | ✅ | Real player chat reaches Takaro with the player attached. |
| Broadcast a message | ✅ | ✅ | ✅ | Shown to everyone in the server chat. |
| Whisper a player | ✅ | ✅ | ✅ | Reaches the intended player. Only one player was online, so "nobody else sees it" was not checked. |
| Give an item | ⚠️ | ⚠️ | ⚠️ | Lands straight in the inventory. Item quality / durability is ignored: everything arrives in default condition. |
| Teleport a player | ✅ | ✅ | ✅ | Goes to the exact coordinates. A spot in the air means the player falls (and can die), so pick a Y on solid ground. |
| Run a console command | ⚠️ | ⚠️ | ⚠️ | The command runs and success comes back, but the command's own output text is always empty. |
| Kick a player | ✅ | ✅ | ✅ | The player is dropped with the reason shown. |
| Ban a player (timed and permanent) | ✅ | ✅ | ✅ | Lands on the server's ban list; the player is refused on rejoin and sees the reason and end time. |
| Unban a player | ✅ | ✅ | ✅ | Clears the ban; the player can join again. |
| Ban list | ✅ | ✅ | ✅ | Matches the server's ban list, including bans placed while the player was offline. |
| Shut the server down | ✅ | ✅ | ✅ | Stops cleanly with all chunks saved. Your host has to start it again. |
| Player joined event | ✅ | ✅ | ✅ | Arrives in Takaro within about a second. |
| Player left event | ✅ | ✅ | ✅ | Also sent for everyone still online when the server shuts down. |
| Player chat event | ✅ | ✅ | ✅ | See "Chat messages from players". |
| Player death event | ✅ | ✅ | ✅ | Includes where the player died. |
| Entity kill event | ✅ | ✅ | ✅ | Includes the entity and the weapon, as game ids (`minecraft:husk`, `minecraft:diamond_sword`). |
| Log events | ❌ | ❌ | ❌ | The connector never sends server log lines to Takaro. |
| Map | ⚠️ | – | – | Off by default. With `map` on and a Takaro that speaks protocol v1, Takaro shows a tiled world map of all three dimensions. takaro.io does not speak v1 yet, so leave it off there. |
| Modules (chat commands) | ✅ | – | – | Tested with the teleports module: `@settp home` and `@tp home` from in-game chat. |
| Discord chat bridge | ✅ | – | – | Both ways: game chat reaches Discord, and Discord messages show in game chat (tested with a simulated Discord message, which takes the same path as a real one). Use the `chatBridgeNoEcho` module; the plain chat bridge echoes Takaro's own messages back. |
| Shop & economy | ✅ | – | – | Granting currency, ordering, and instant delivery into the inventory work; an order without enough currency is refused. |

Modules, Discord and the shop run in Takaro and only use the chat, message and give-item features
above, which work on all three platforms.

### Known issues

- **Fixed after 0.5.0.** In 0.5.0 and older, Paper and NeoForge did not report the players still
  online as having left when the server shut down (Takaro kept showing them online), and a ban
  placed while the player was offline showed no player name in the ban list. Both are fixed in the
  release after 0.5.0; the table above describes that release.
- **Item quality is ignored.** Everything the shop or Takaro hands out arrives in default
  condition; there is no durability or quality control.
- **Console command output is not returned.** Check the effect in game instead.
- **Map needs protocol v1.** `map=true` only works with a Takaro that speaks protocol v1, which
  takaro.io does not yet.
- **No locations, no log events.** Minecraft has no named-location list for the connector to
  report, and server log lines are not forwarded.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
