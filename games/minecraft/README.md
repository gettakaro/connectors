# Takaro Minecraft Connector

Connects a Minecraft dedicated server to Takaro with one server-side file for **Fabric**,
**Paper** or **NeoForge**. Players do not install anything and join with the normal Minecraft
client.

## Install

### 1. Before you start

- A Minecraft dedicated server you can stop, start and copy files to, running one of:
  - **Fabric** on Minecraft 26.2, 26.1.2 or 26.3 with Java 25, the Fabric API mod and Fabric Loader
    0.19.5 or newer (0.19.3 on 26.1.2).
  - **Paper** on Minecraft 1.21.11 with Java 21.
  - **NeoForge** 21.11.45 or newer on Minecraft 1.21.11 with Java 21.
- A Takaro game server of type **Generic** and its registration token.

Players' clients must match the server's Minecraft version (a 26.2 client cannot join a 1.21.11
server, and the other way round).

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

### 2. Download

Download the latest release from https://takaro.io/connectors/minecraft (or the latest
`minecraft-v<version>` release on https://github.com/gettakaro/connectors/releases). Take the
**one** file that matches your server:

| Your server | File to download |
|---|---|
| Fabric 26.2 | `takaro-minecraft-mod-fabric-26.2-<version>.jar` |
| Fabric 26.1.2 | `takaro-minecraft-mod-fabric-26.1.2-<version>.jar` |
| Fabric 26.3 | `takaro-minecraft-mod-fabric-26.3-<version>.jar` |
| Paper 1.21.11 | `takaro-minecraft-mod-paper-1.21.11-<version>.jar` |
| NeoForge 1.21.11 | `takaro-minecraft-mod-neoforge-1.21.11-<version>.jar` |

Check it against `SHA256SUMS`. Do not use the `minecraft-dev` pre-release. Older releases used
other file names; see [UPGRADING.md](UPGRADING.md).

### 3. Install

Stop the server and put the jar in the folder for your platform:

```text
Fabric     <server>/mods/takaro-minecraft-mod-fabric-26.2-<version>.jar
NeoForge   <server>/mods/takaro-minecraft-mod-neoforge-1.21.11-<version>.jar
Paper      <server>/plugins/takaro-minecraft-mod-paper-1.21.11-<version>.jar
```

No extra libraries are needed. On Fabric, keep the Fabric API jar in `mods/` next to it.

### 4. Configure

Start the server. The connector writes its config file on the first start (or create it yourself
beforehand with the content below):

| Your server | Config file |
|---|---|
| Fabric | `<server>/config/takaro.json` |
| NeoForge | `<server>/config/takaro.properties` |
| Paper | `<server>/plugins/TakaroMinecraft/config.yml` |

Paste your registration token into `registration_token` and save. The connector checks the file
every few seconds and connects, no restart needed. It fills in `identity_token` itself; keep it
when you move or copy the server, it is how Takaro knows this server.

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
    "map": false,
    "server_chat_name": ""
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
# World map in Takaro. Needs a Takaro that speaks protocol v1.
takaro.map=false
# Sender name on Takaro's messages when Takaro sends none. Empty = no sender name.
takaro.server_chat_name=
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
  # World map in Takaro. Needs a Takaro that speaks protocol v1.
  map: false
  # Sender name on Takaro's messages when Takaro sends none. Empty = no sender name.
  server_chat_name: ""
```

`map` (Fabric: `settings.map`; or `TAKARO_MAP=true`) turns on the world map in Takaro. It needs a
Takaro with map support. Changing it reconnects, no restart needed.

**Server chat name (optional).** Messages from Takaro show as `[Name] text` in global chat and `Name whispers to you: text` in a whisper. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `server_chat_name` (Fabric: `settings.server_chat_name`; or `TAKARO_SERVER_CHAT_NAME`) instead; the name set in Takaro wins over it. With no name, the message is the bare text.

### 5. Check that it works

Before a token is set, the server console (and `logs/latest.log`) shows a banner naming the file to
edit:

```text
*************************************************************************
  registration_token not set, the server is not connected to Takaro.
  Paste the registration token from Takaro into /srv/mc/config/takaro.json
  and save it. The connector connects within a few seconds, no restart needed.
*************************************************************************
```

A few seconds after you save the token (Paper prefixes these lines with `[TakaroMinecraft]`,
NeoForge with `[Takaro/]`):

```text
Takaro target-check: {"target":"fabric-26.2", ... "result":"ok","reasons":[]}
Connecting to Takaro at wss://connect.takaro.io/
Identified successfully, server ID: <your server id>, protocol v1
```

The game server now shows as online in Takaro. A wrong token shows a banner starting with
`Identify failed:`; fix it and save.

If the jar does not match the server, the connector refuses to connect:

```text
Takaro refuses to connect: target fabric-26.2 does not match this server
(built for game version 26.2, this server runs 26.1.2).
```

Download the jar for your server's version instead (see [UPGRADING.md](UPGRADING.md)).

### 6. Upgrade

Stop the server, delete the old Takaro jar from `mods/` (or `plugins/`), put the new jar in its
place, and start the server. Keep the config file: your token and identity stay. Never swap the jar
under a running server.

## What works, what doesn't

Checked on Fabric 26.2, Paper 1.21.11 and NeoForge 1.21.11, 2026-10-22, and Fabric 26.3, 2026-10-26. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects on its own after a restart. |
| Player list | ✅ | Name, id and ping. |
| Player location | ✅ | Rounded to whole blocks. |
| Teleport | ✅ | To the exact coordinates; pick a Y on solid ground or the player falls. |
| Player inventory | ✅ | Changes are reported as they happen. |
| Item list | ✅ | About 1,500 items with their in-game names. |
| Entity list | ✅ | Paper lists the mob types; Fabric and NeoForge list every entity type, including things like Primed TNT. |
| Locations | ❌ | Minecraft has no list of named locations. |
| World map | ⚠️ | Off by default (`map` setting). Tiled map of all three dimensions. Not checked on takaro.io yet. Checked on Fabric only. |
| Send a message | ✅ | Broadcasts and whispers, as `[Name] text` and `Name whispers to you: text` when a server chat name is set. |
| Console commands | ⚠️ | The command runs, but its output text is not returned. |
| Kick | ✅ | The reason is shown to the player. |
| Ban and unban | ✅ | Timed and permanent, on the server's own ban list. |
| Ban list | ✅ | Includes bans placed while the player was offline. |
| Give an item | ⚠️ | Straight into the inventory, but always in default condition (quality/durability is ignored). |
| Shop | ✅ | Orders deliver into the inventory; not enough currency is refused. Checked on Fabric. |
| Shut down the server | ✅ | Saves and stops; your host has to start it again. |
| Player joined / left | ✅ | Everyone still online is reported as left at shutdown. |
| Chat messages | ✅ | |
| Player death | ✅ | With the position. |
| Entity killed | ✅ | Entity and weapon as game ids, for example `minecraft:husk`. |
| Server log lines | ❌ | Not sent to Takaro. |
| Discord chat bridge | ✅ | Both ways. Use the `chatBridgeNoEcho` module; the plain chat bridge echoes Takaro's own messages. Checked on Fabric. |
| Modules | ✅ | Chat commands such as `@settp home` and `@tp home`. Checked on Fabric. |
| Game name, version and icon | ✅ | Takaro shows Minecraft with its icon and the running game version. |

## Known issues

- The Fabric 26.1.2 file is built and checked the same way as the others, but not played on.
- Items given by Takaro or the shop always arrive in default condition.
- Console command output is not returned; check the effect in game.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
