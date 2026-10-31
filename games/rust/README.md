# Takaro Rust Connector

One plugin file for **Carbon** or **Oxide/uMod** that connects a Rust dedicated server (Linux or
Windows) to Takaro. Players do not install anything.

## Install

You need: a Rust server with [Carbon](https://carbonmod.gg/) or
[Oxide/uMod](https://umod.org/games/rust), and a Takaro game server of type **Generic** with its
registration token. Nothing needs to be port-forwarded.

### 1. Download

From https://takaro.io/connectors/rust, download **`TakaroConnector.cs`**. Do not use the
`rust-dev` pre-release.

### 2. Install

Save it **as `TakaroConnector.cs`** (the name matters) in the plugin folder:

```text
Carbon:  <server>/carbon/plugins/TakaroConnector.cs
Oxide:   <server>/oxide/plugins/TakaroConnector.cs
```

The framework compiles and loads it right away; no restart needed.

### 3. Configure

On first load the plugin creates its config file:

```text
Carbon:  <server>/carbon/configs/TakaroConnector.json
Oxide:   <server>/oxide/config/TakaroConnector.json
```

Paste your registration token and save. The plugin picks it up within 5 seconds.

```json
{
  "RegistrationToken": "your-registration-token-here",
  "IdentityToken": "3f2b0c1e-...",
  "WebSocketUrl": "wss://connect.takaro.io/",
  "Debug": false,
  "ServerChatName": ""
}
```

Do not change `IdentityToken`, or Takaro sees a new server.

| Optional setting | What it does |
|---|---|
| `ServerChatName` | Sender name on messages from Takaro, used when none is set for the server in Takaro. |
| `Debug` | `true` logs every message sent and received. |

### 4. Check that it works

The server console shows `[Takaro] Identified successfully, server ID: <id>` and the server shows
as online in Takaro. Without a token, a banner names the file to fix; a wrong token shows
`Takaro rejected identify: Invalid registrationToken provided.`

**Upgrading:** replace `TakaroConnector.cs` and keep `TakaroConnector.json`.

## What works, what doesn't

Checked on Rust build 26139947 with Carbon v2.0.262, 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects after a server restart. |
| Player list | ✅ | Steam id, IP and ping. |
| Player location | ✅ | |
| Teleport | ✅ | No ground snapping; pick a safe height. |
| Player inventory | ⚠️ | Item quality is always empty. |
| Item list | ✅ | |
| Entity list | ✅ | |
| Locations | ⚠️ | The map's monuments. Not checked. |
| World map | ⚠️ | Drawn from the game's map. Not checked yet. |
| Send a message | ✅ | Broadcasts and whispers. |
| Console commands | ⚠️ | An unknown command comes back as a success. |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent. |
| Ban list | ✅ | |
| Give an item | ✅ | Dropped at the player's feet when the inventory is full. |
| Shop | ✅ | |
| Shut down the server | ✅ | Saves first. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ✅ | Killer only when it was a player. |
| Entity killed | ✅ | With the weapon. |
| Server log lines | ⚠️ | Takaro does not keep them. |
| Discord chat bridge | ✅ | Both ways. |
| Modules | ✅ | |
| Game name, version and icon | ✅ | Not checked yet. |
| Oxide/uMod | ⚠️ | Written for Oxide, but only run on Carbon. |

## Known issues

- The first start on a new map pauses the server for a few seconds while it draws the world map.
- Only Rust build 26139947 with Carbon v2.0.262 is checked; other builds likely work.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
