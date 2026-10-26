# Takaro Minecraft Connector

A server-side connector that links a Minecraft Java dedicated server running **Fabric**, **Paper**
or **NeoForge** to Takaro. It is a single jar per server build. Players do not install anything and
join with the normal Minecraft client.

## Install

### 1. Check your server

You need a Takaro game server of type **Generic** with its **registration token**, and one of:

- **Fabric** on Minecraft 26.1.2, 26.2 or 26.3, Java 25, with the **Fabric API** mod (Fabric Loader
  0.19.5 or newer; 0.19.3 on 26.1.2).
- **Paper** on Minecraft 1.21.11, Java 21.
- **NeoForge** 21.11.45 or newer on Minecraft 1.21.11, Java 21.

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `fabric-26.1.2` | 26.1.2 | fabric | loader 0.19.3 / API 0.155.3+26.1.2 | 25 | maintained | protocol |
| `fabric-26.2` | 26.2 | fabric | loader 0.19.5 / API 0.160.0+26.2 | 25 | maintained | protocol |
| `fabric-26.3` | 26.3 | fabric | loader 0.19.5 / API 0.161.0+26.3 | 25 | maintained | protocol |
| `neoforge-1.21.11` | 1.21.11 | neoforge | loader 21.11.45 | 21 | maintained | protocol |
| `paper-1.21.11` | 1.21.11 | paper | loader 132 | 21 | maintained | protocol |
<!-- takaro-maint:targets:end -->

Players' clients must match the server's Minecraft version.

### 2. Download

From https://takaro.io/connectors/minecraft (or the `minecraft-v<version>` release on
[GitHub](https://github.com/gettakaro/connectors/releases)), download the **one** jar for your
server: `takaro-minecraft-mod-<target>-<version>.jar`, where `<target>` is `fabric-26.3`,
`fabric-26.2`, `fabric-26.1.2`, `paper-1.21.11` or `neoforge-1.21.11`. Do not use the untested
`minecraft-dev` pre-release. Coming from a release before 0.5.0? See [UPGRADING.md](UPGRADING.md).

### 3. Copy it into place

Stop the server and put the jar in:

- Fabric and NeoForge: `<server>/mods/` (on Fabric, keep the Fabric API jar next to it)
- Paper: `<server>/plugins/`

### 4. Set the registration token

Start the server. The connector creates its config file and shows a `registration_token not set`
banner naming it:

| Server | Config file | Token setting |
|---|---|---|
| Fabric | `<server>/config/takaro.json` | `authentication.registration_token` |
| NeoForge | `<server>/config/takaro.properties` | `takaro.authentication.registration_token` |
| Paper | `<server>/plugins/TakaroMinecraft/config.yml` | `takaro.authentication.registration_token` |

Paste the token and save. The connector connects within a few seconds; no restart needed. Leave
`identity_token` as the connector filled it in; it is how Takaro knows this server.

Optional: set `map` to `true` (Fabric: `settings.map`; NeoForge: `takaro.map`; Paper:
`takaro.map`) to show the world map in Takaro.

### 5. Check that it worked

The server log shows:

```
Identified successfully, server ID: <your server id>, protocol v1
```

and Takaro shows the server as online. A wrong token logs `Identify failed:`. A jar for another
Minecraft version logs `Takaro refuses to connect: target ... does not match this server`;
download the matching jar.

### Updating

Stop the server, replace the Takaro jar with the new one and start the server. The config file is
kept, so the server keeps its place in Takaro.

## What works, what doesn't

Tested on Fabric 26.3, Paper 1.21.11 and NeoForge 1.21.11 on 2026-10-26, and on Fabric 26.2 on
2026-10-22. Fabric 26.1.2 is built and checked but was not played on.

| Feature | Status | Note |
|---|---|---|
| Connection and reconnect | ✅ | Comes back by itself after a server restart. |
| Game profile | ✅ | Minecraft name, icon, game version and connector version. |
| Player list and lookup | ✅ | Name, Minecraft UUID and ping. |
| Player location | ✅ | Rounded to whole blocks. |
| Player inventory | ✅ | |
| Item catalogue | ✅ | In-game names. |
| Entity catalogue | ⚠️ | Fabric and NeoForge also list non-creatures (boats, fireballs). |
| Locations | ❌ | Minecraft has no named locations; always empty. |
| World map | ✅ | Off by default; all three dimensions. Tested on Fabric. |
| Broadcast a message | ✅ | |
| Whisper a player | ✅ | Reaches the player; not tested with two players. |
| Give an item | ⚠️ | Items arrive in default condition; quality and durability are ignored. |
| Teleport a player | ✅ | Exact coordinates; refused for a dead or offline player. |
| Run a console command | ⚠️ | Runs, but output is always empty, even for unknown commands. |
| Kick a player | ✅ | |
| Ban, unban, ban list | ✅ | Timed and permanent; timed bans expire on time. |
| Shut the server down | ✅ | Saves and stops; your host must start it again. |
| Player joined / left | ✅ | |
| Chat | ✅ | |
| Player death | ✅ | With position. |
| Entity kill | ✅ | Entity and weapon as game ids (`minecraft:zombie`). |
| Log events | ❌ | Server log lines are not sent. |
| Modules, economy and shop | ✅ | Shop tested on Fabric; purchases go straight into the inventory. |
| Discord chat bridge | ✅ | Both ways; use the `chatBridgeNoEcho` module. |

## Known issues

- In 0.5.0, Paper and NeoForge do not report online players as left on shutdown. Fixed in the next release.
- In 0.5.0, the first rejoin after a timed ban expires can fail on Fabric and NeoForge. Fixed in the next release.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
