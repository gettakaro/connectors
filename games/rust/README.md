# Takaro Rust Connector

A server-side plugin that connects a Rust dedicated server to Takaro. It runs on
[Carbon](https://carbonmod.gg/) and is written for the [Oxide/uMod](https://umod.org/games/rust) plugin API.
Players do not install anything.

## Install

### 1. Before you start

- A Rust dedicated server (Linux or Windows) with Carbon or Oxide/uMod installed, and access to its files.
- A Takaro game server of type **Generic** and its **registration token**.
- Outbound access to `wss://connect.takaro.io/`. No port forwarding is needed.

### 2. Download

Download **`TakaroConnector.cs`** from the latest `rust-vX.Y.Z` release on
<https://takaro.io/connectors/rust> or <https://github.com/gettakaro/connectors/releases>. The same file is also
published under its target name (`takaro-rust-plugin-carbon-<build>-<version>.cs`). Do not use the `rust-dev`
pre-release.

### 3. Copy it into the plugin folder

Save it as `TakaroConnector.cs` (the name matters):

```
<server>/carbon/plugins/TakaroConnector.cs    # Carbon
<server>/oxide/plugins/TakaroConnector.cs     # Oxide / uMod
```

The framework compiles and loads it right away; no restart is needed. On first load the plugin creates
`carbon/configs/TakaroConnector.json` (Carbon) or `oxide/config/TakaroConnector.json` (Oxide).

### 4. Set the registration token

Paste the token into `RegistrationToken` in `TakaroConnector.json` and save. The plugin re-reads the file every
5 seconds and connects; no restart.

| Field | What it is |
|---|---|
| `RegistrationToken` | Your Takaro registration token. Required. |
| `IdentityToken` | Generated on first load. Do not change it, or Takaro sees a new server. |
| `Debug` | `true` logs every message sent and received. |

Environment variables override the file: `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_WS_URL`,
`TAKARO_DEBUG` (changing them needs a server restart).

### 5. Check that it worked

The server console shows:

```
[Takaro] WebSocket connected
[Takaro] Identified successfully, server ID: <id> (protocol 1)
```

and Takaro shows the server as online. A missing or wrong token prints a banner naming the file to fix.

To update, replace `TakaroConnector.cs`; the plugin reloads by itself and keeps its config.

## What works, what doesn't

Tested on Rust build 26139947 with Carbon v2.0.262 on 2026-10-25.

| Feature | Status | Note |
|---|---|---|
| Connect, reachability, reconnect after restart | ✅ | |
| Token change without restart | ✅ | Picked up within 5 seconds |
| Game profile (name, version, icon) | ✅ | Needs Takaro protocol 1. |
| Player list and lookup | ✅ | Steam ID, IP and ping |
| Player location, teleport | ✅ | Teleport uses exact height, no ground snapping |
| Player inventory | ✅ | Item quality is always empty |
| Item and entity lists | ✅ | Display names, animals included |
| Locations | ✅ | Monuments; needs protocol v1 |
| Give item | ✅ | Drops at the player's feet when inventory is full |
| Send message, whisper | ✅ | |
| Console commands | ⚠️ | Unknown commands return success with empty output |
| Kick, ban, timed ban, unban, ban list | ✅ | Timed bans are lifted at expiry |
| Shutdown | ✅ | Saves and quits after answering Takaro |
| Join, leave, chat events | ✅ | |
| Player death event | ✅ | Killer only when another player did it |
| Entity killed event | ✅ | Animals and NPCs, with the weapon |
| Log events | ⚠️ | Sent, but Takaro does not store them |
| Modules, shop, economy | ✅ | |
| Discord chat bridge | ✅ | Both ways |
| Map and map tiles | ⚠️ | Needs Takaro protocol 1. |
| Oxide / uMod | ⚠️ | Written for it, only Carbon is tested |
| Second player (whisper isolation) | ⚠️ | Not tested with two players |

## Known issues

- The first start on a new map pauses the server for several seconds while it draws the world map.
- Releases 0.0.4 to 0.1.0 made every ban permanent; update to a newer release.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
