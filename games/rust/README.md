# Takaro Rust Connector

A server-side-only plugin that connects a Rust dedicated server to Takaro. It is written for the
**Oxide/uMod** plugin API (`Oxide.Plugins` / `RustPlugin`) and is developed and verified on
**Carbon**, which runs the same plugins. Players do not install anything.

**Tested live against Rust public build 25653776 (the 2026-10-01 "Livestock" update) with Carbon
v2.0.261, with a real game client.** Other Rust builds and other Carbon builds are unverified — the plugin will very likely
still load, but nothing here was checked against them.

## Install

### 1. Before you start

You need:

- A **Rust dedicated server** (Linux or Windows) that you can stop, start and copy files to, with
  either **[Carbon](https://carbonmod.gg/)** or **[Oxide/uMod](https://umod.org/games/rust)**
  installed.
- Access to the server's **plugin folder** (`carbon/plugins/` on Carbon, `oxide/plugins/` on Oxide).
- The ability to set **environment variables** on the server process — this plugin is configured
  through the environment, not through a config file (see step 4).
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

### 2. Download the plugin

Download the latest release: https://takaro.io/connectors/rust

From the latest `rust-vX.Y.Z` release on the releases page

> https://github.com/gettakaro/connectors/releases

download either name — they are the same bytes:

- **`takaro-rust-plugin-carbon-25653776-<version>.cs`** — the build's own name, which says exactly
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

The framework picks the file up and compiles it on the spot; a dropped-in plugin does not need a
server restart, but the environment variables in step 4 do.

### 4. Configure

**The plugin writes no config file.** It reads four environment variables from the server process
when it loads:

| Variable | What it is | Default |
|---|---|---|
| `TAKARO_REGISTRATION_TOKEN` | Your Takaro registration token. **Required.** | (none) |
| `TAKARO_WS_URL` | Takaro WebSocket endpoint. Leave as is. | `wss://connect.takaro.io/` |
| `TAKARO_IDENTITY_TOKEN` | A unique name for this server. Use a different one per server. | (empty) |
| `TAKARO_DEBUG` | `true` to log every message sent and received. | `false` |

Set them where your server process gets its environment — for example in the systemd unit, in the
start script before launching `RustDedicated`, or as `environment:` entries in Docker Compose:

```bash
export TAKARO_REGISTRATION_TOKEN="your-registration-token-here"
export TAKARO_IDENTITY_TOKEN="my-rust-server-1"
```

Then restart the server so the process picks up the new environment.

### 5. Check that it worked

In the server console / Carbon or Oxide log, the plugin prints lines prefixed with `[Takaro]`:

```
Loaded plugin TakaroConnector v<version> by Takaro [1667ms]
[Takaro] Connecting to wss://connect.takaro.io/
[Takaro] WebSocket connected
[Takaro] Identified successfully, server ID: <id>
```

And in Takaro, the game server shows as **online**.

If instead you see:

```
TAKARO_REGISTRATION_TOKEN not set. Plugin will not connect.
```

then the server process did not get the environment variable — re-check step 4. If it connects but
never identifies, the registration token is the first thing to re-check.

### 6. Upgrading

Replace `TakaroConnector.cs` in the plugin folder with the new file. Carbon and Oxide notice the
changed file and reload the plugin by themselves; no server restart is needed. Your environment
variables are untouched by the upgrade, so the server keeps its identity.

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
