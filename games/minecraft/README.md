# Takaro Minecraft Connector

One server-side file for **Fabric**, **Paper** or **NeoForge** that connects a Minecraft dedicated
server to Takaro. Players do not install anything and join with the normal client.

## Install

You need: one of the servers below, and a Takaro game server of type **Generic** with its
registration token.

- **Fabric** on 26.2, 26.1.2 or 26.3 with Java 25, Fabric API and Fabric Loader 0.19.5 or newer
  (0.19.3 on 26.1.2).
- **Paper** 1.21.11 with Java 21.
- **NeoForge** 21.11.45 or newer on 1.21.11 with Java 21.

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `fabric-26.1.2` | 26.1.2 | fabric | loader 0.19.3 / API 0.155.3+26.1.2 | 25 | maintained | protocol |
| `fabric-26.2` | 26.2 | fabric | loader 0.19.5 / API 0.160.0+26.2 | 25 | maintained | protocol |
| `fabric-26.3` | 26.3 | fabric | loader 0.19.5 / API 0.161.0+26.3 | 25 | maintained | protocol |
| `neoforge-1.21.11` | 1.21.11 | neoforge | loader 21.11.45 | 21 | maintained | protocol |
| `paper-1.21.11` | 1.21.11 | paper | loader 132 | 21 | maintained | protocol |
<!-- takaro-maint:targets:end -->

### 1. Download

From https://takaro.io/connectors/minecraft, download the **one** jar for your server:

| Your server | File |
|---|---|
| Fabric 26.2 | `takaro-minecraft-mod-fabric-26.2-<version>.jar` |
| Fabric 26.1.2 | `takaro-minecraft-mod-fabric-26.1.2-<version>.jar` |
| Fabric 26.3 | `takaro-minecraft-mod-fabric-26.3-<version>.jar` |
| Paper 1.21.11 | `takaro-minecraft-mod-paper-1.21.11-<version>.jar` |
| NeoForge 1.21.11 | `takaro-minecraft-mod-neoforge-1.21.11-<version>.jar` |

Do not use the `minecraft-dev` pre-release.

### 2. Install

Stop the server and put the jar in `<server>/mods/` (Fabric, NeoForge) or `<server>/plugins/`
(Paper). On Fabric, keep the Fabric API jar in `mods/` too.

### 3. Configure

Start the server once; the connector writes its config file. Paste your registration token and
save. It connects within a few seconds, no restart needed.

| Your server | Config file | Token key |
|---|---|---|
| Fabric | `config/takaro.json` | `authentication.registration_token` |
| NeoForge | `config/takaro.properties` | `takaro.authentication.registration_token` |
| Paper | `plugins/TakaroMinecraft/config.yml` | `takaro.authentication.registration_token` |

For example, on Fabric:

```json
"authentication": {
  "identity_token": "",
  "registration_token": "your-registration-token-here"
}
```

The connector fills in `identity_token`; keep it when you move the server.

| Optional setting | What it does |
|---|---|
| `map` (Fabric: `settings.map`) | `true` turns on the world map in Takaro. |
| `server_chat_name` (Fabric: `settings.server_chat_name`) | Sender name on messages from Takaro, used when none is set for the server in Takaro. |

### 4. Check that it works

The console shows `Identified successfully, server ID: <id>` and the server shows as online in
Takaro. Without a token, a banner names the file to edit. A wrong token shows
`Identify failed:`; a jar for the wrong game version shows `Takaro refuses to connect: target ...
does not match this server`.

**Upgrading:** stop the server and replace the old Takaro jar with the new one; keep the config
file. Older releases used other file names; see [UPGRADING.md](UPGRADING.md).

## What works, what doesn't

Checked on Fabric 26.2, Paper 1.21.11 and NeoForge 1.21.11, 2026-10-22, and Fabric 26.3, 2026-10-26. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects after a restart. |
| Player list | ✅ | Name, id and ping. |
| Player location | ✅ | |
| Teleport | ✅ | To the exact coordinates; pick a Y on solid ground. |
| Player inventory | ✅ | |
| Item list | ✅ | |
| Entity list | ✅ | |
| Locations | ❌ | Minecraft has no named locations. |
| World map | ⚠️ | Off by default (`map` setting). Checked on Fabric only. |
| Send a message | ✅ | Broadcasts and whispers. |
| Console commands | ⚠️ | Runs, but the output is not returned. |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent. |
| Ban list | ✅ | |
| Give an item | ⚠️ | Always in default condition. |
| Shop | ✅ | Checked on Fabric. |
| Shut down the server | ✅ | Saves and stops; your host restarts it. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | |
| Entity killed | ✅ | As game ids, for example `minecraft:husk`. |
| Server log lines | ❌ | Not sent. |
| Discord chat bridge | ✅ | Use `chatBridgeNoEcho`. Checked on Fabric. |
| Modules | ✅ | Checked on Fabric. |
| Game name, version and icon | ✅ | |

## Known issues

- Items given by Takaro or the shop always arrive in default condition.
- Console command output is not returned; check the effect in game.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
