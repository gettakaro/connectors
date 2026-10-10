# Takaro Rust Connector

A server-side-only plugin that connects a Rust dedicated server to Takaro. It is written for the
**Oxide/uMod** plugin API (`Oxide.Plugins` / `RustPlugin`) and is developed and verified on
**Carbon**, which runs the same plugins. Players do not install anything.

**Tested live against Rust public build 25823813 (the 2026-10-09 update) with Carbon v2.0.262,
with a real game client.** Other Rust builds and other Carbon builds are unverified — the plugin will very likely
still load, but nothing here was checked against them.

## Install

### 1. Before you start

You need:

- A **Rust dedicated server** (Linux or Windows) that you can stop, start and copy files to, with
  either **[Carbon](https://carbonmod.gg/)** or **[Oxide/uMod](https://umod.org/games/rust)**
  installed.
- Access to the server's **plugin folder** (`carbon/plugins/` on Carbon, `oxide/plugins/` on Oxide).
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

### 2. Download the plugin

Download the latest release: https://takaro.io/connectors/rust

From the latest `rust-vX.Y.Z` release on the releases page

> https://github.com/gettakaro/connectors/releases

download either name — they are the same bytes:

- **`takaro-rust-plugin-carbon-25823813-<version>.cs`** — the build's own name, which says exactly
  which Rust build and which Carbon it was verified against.
- **`TakaroConnector.cs`** — the same file under the name the framework loads. Direct link pattern:
  `https://github.com/gettakaro/connectors/releases/download/rust-v<version>/TakaroConnector.cs`

`SHA256SUMS` is published beside them if you want to check the download.

Use a release newer than `rust-v0.1.0` (older ones turn every timed ban into a permanent one). Do not use the `rust-dev` pre-release; that is an untested rolling
build. Or copy the file straight out of the repository: `games/rust/mod/TakaroConnector.cs`.

There is nothing to compile — Carbon and Oxide compile `.cs` plugins at runtime.

### 3. Copy it into place

Save the file **as `TakaroConnector.cs`** in your framework's plugin folder — the framework loads a
plugin by its class-named file, so the name matters:

```
# Carbon
<server>/carbon/plugins/TakaroConnector.cs

# Oxide / uMod
<server>/oxide/plugins/TakaroConnector.cs
```

Examples:

- Linux (Carbon): `/home/steam/rust/carbon/plugins/TakaroConnector.cs`
- Windows (Oxide): `C:\RustServer\oxide\plugins\TakaroConnector.cs`

The framework picks the file up and compiles it on the spot; no server restart is needed. On its
first load the plugin creates its config file:

```
# Carbon
<server>/carbon/configs/TakaroConnector.json

# Oxide / uMod
<server>/oxide/config/TakaroConnector.json
```

### 4. Paste the registration token

Open `TakaroConnector.json`, paste the registration token from Takaro between the quotes of
`RegistrationToken`, and save the file. The plugin re-reads the file every 5 seconds and connects
on its own; no restart and no plugin reload.

```json
{
  "RegistrationToken": "your-registration-token-here",
  "IdentityToken": "3f2b0c1e-...",
  "WebSocketUrl": "wss://connect.takaro.io/",
  "Debug": false
}
```

| Field | What it is |
|---|---|
| `RegistrationToken` | Your Takaro registration token. **Required.** |
| `IdentityToken` | This server's unique name in Takaro. Generated on first load; do not change it, or Takaro sees a new server. |
| `WebSocketUrl` | Takaro WebSocket endpoint. Leave as is. |
| `Debug` | `true` logs every message sent and received. |

**Environment variables** (Docker, systemd) still work and win over the file, field by field:
`TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_WS_URL`, `TAKARO_DEBUG`. A server's
environment is fixed when it starts, so changing one of those needs a server restart.

### 5. Check that it worked

In the server console / Carbon or Oxide log, the plugin prints lines prefixed with `[Takaro]`:

```
[Takaro] Config: <server>/carbon/configs/TakaroConnector.json (registration token set)
[Takaro] Connecting to wss://connect.takaro.io/
Loaded plugin TakaroConnector v<version> by Takaro [1667ms]
[Takaro] WebSocket connected
[Takaro] Identified successfully, server ID: <id>
```

And in Takaro, the game server shows as **online**.

If the token is missing or wrong, the console shows a banner naming the exact file to fix:

```
*************************************************************************
  RegistrationToken not set, the server is not connected to Takaro.
  Paste the registration token from Takaro into <server>/carbon/configs/TakaroConnector.json
  and save it. The plugin connects within a few seconds, no restart needed.
*************************************************************************
```

or `Takaro rejected identify: Invalid registrationToken provided.` Save a corrected token and it
reconnects within a few seconds.

### 6. Upgrading

Replace `TakaroConnector.cs` in the plugin folder with the new file. Carbon and Oxide notice the
changed file and reload the plugin by themselves; no server restart is needed. Leave
`TakaroConnector.json` where it is: the new version reads it, so the server keeps its token and its
identity. A server that was set up with environment variables before this config file existed keeps
the identity it had; the file is created with an empty `IdentityToken` and the environment still
wins.

## What works, what doesn't

✅ means it was proven live on 2026-10-01 with a real Rust game client joined to the server and every
result checked in Takaro and in the game — first on build 25454815 with Carbon 2.0.259, then again
on build 25653776 with Carbon 2.0.261 after that day's update.
⚠️ means the plugin implements it but it has not been confirmed live. ❌ means it is not
implemented or not supported.

| What | | Notes |
|---|---|---|
| Plugin compiles and loads | ✅ | Carbon compiles the `.cs` at load, and reloads it by itself when the file changes. |
| Connection & identify | ✅ | Connects outbound over WebSocket and identifies to Takaro. |
| Heartbeat / reachability | ✅ | Answers Takaro's reachability check. |
| Server restart / reconnect | ✅ | Reconnects on its own after the server comes back, and identifies again. |
| Config file, token change without restart | ✅ | Proven 2026-10-10 on Carbon: created on first load, a pasted or corrected token connects within 5 s, an upgrade keeps token and identity. |
| Player list | ✅ | The connected players, with Steam id, IP and ping. |
| Single player lookup | ✅ | Finds connected and sleeping players by Steam id. |
| Player location | ✅ | Live position; Takaro's map tracking follows the player. |
| Player inventory | ✅ | Main inventory, hotbar and worn items; Takaro records items added and removed. Item quality is always empty. |
| Item catalogue | ✅ | Every item the server knows, with its display name (e.g. `rifle.ak` → "Assault Rifle"). |
| Entity catalogue | ✅ | Animals and NPCs with display names (e.g. `scientistnpc_roam` → "Roaming Scientist"). |
| Run a console command | ✅ | Returns the command's output. A command that fails in Rust comes back as failed, with Rust's error text. An unknown command comes back as a success with empty output. |
| Broadcast a message | ✅ | Shows in every player's chat. |
| Whisper a player | ✅ | Shows in that player's chat only. |
| Give an item | ✅ | Goes straight into the player's inventory; if there is no room it drops at their feet. |
| Teleport a player | ✅ | Moves the player to the exact coordinates given, with no ground snapping — pick a safe height. |
| Kick | ✅ | Drops the player with the reason shown. |
| Ban (timed) | ✅ | The player is kicked, cannot rejoin until the expiry, and can rejoin once it passes. |
| Ban (permanent) | ✅ | Goes into the server's own ban list; the player is kicked. |
| Unban | ✅ | Clears the ban; the player can rejoin. |
| Ban list | ✅ | Timed bans with their real expiry, permanent bans with none. |
| Shut the server down | ✅ | Answers Takaro first, then saves and quits two seconds later. |
| Player joined event | ✅ | |
| Player left event | ✅ | Also sent when a player is kicked or banned. |
| Player chat event | ✅ | With the player and the chat channel. |
| Player death event | ✅ | With the position; includes the killer only when another player did it. |
| Entity kill event | ✅ | When a player kills an animal or NPC, with the weapon used. Kill rewards pay out. |
| Log events | ⚠️ | Server console lines are forwarded, minus Carbon's and the plugin's own. Takaro does not store them as events. |
| Chat commands (modules) | ✅ | Commands like `help`, `ping`, `balance`, `settp` and `tp` run and answer in game. |
| Welcome message & scheduled messages | ✅ | Both show in game. |
| Shop & economy | ✅ | Currency, buying a listing and delivering it into the player's inventory. |
| Discord chat bridge | ✅ | Both ways: game chat appears in Discord, Discord messages appear in game. |
| Oxide / uMod | ⚠️ | Written against the Oxide plugin API, but only Carbon has been run. |
| Map info | ❌ | The plugin does not implement it. |
| Map tiles | ❌ | Not supported by Takaro for Generic-connector servers. |

### Known issues

- **No map.** Takaro's API does not support map tiles for Generic-connector servers, and the plugin
  does not answer map-info requests either.
- **Item quality is not reported.** Inventory entries always come back with an empty quality field.
- **Unknown console commands look successful.** Rust gives no error for a command it does not know.
- **Releases 0.0.4 to 0.1.0 made every ban permanent.** Update to get working timed bans.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
