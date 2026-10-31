# Takaro Rust Connector

Connects a Rust dedicated server (Linux or Windows) to Takaro with one plugin file for
**Carbon** or **Oxide/uMod**. Players do not install anything.

## Install

### 1. Before you start

- A Rust dedicated server you can stop, start and copy files to, with
  [Carbon](https://carbonmod.gg/) or [Oxide/uMod](https://umod.org/games/rust) installed.
- Access to the server's plugin folder (`carbon/plugins/` on Carbon, `oxide/plugins/` on Oxide).
- Outbound access to `wss://connect.takaro.io/`. Nothing needs to be port-forwarded.
- A Takaro game server of type **Generic** and its registration token.

### 2. Download

Download the latest release from https://takaro.io/connectors/rust (or the latest `rust-v<version>`
release on https://github.com/gettakaro/connectors/releases): **`TakaroConnector.cs`**. The same
file is also published under its target name, for example
`takaro-rust-plugin-carbon-26139947-<version>.cs`, which names the Rust build it was made for. Check
it against `SHA256SUMS`. Do not use the `rust-dev` pre-release.

There is nothing to compile: Carbon and Oxide compile `.cs` plugins at runtime.

### 3. Install

Save the file **as `TakaroConnector.cs`** (the name matters) in your plugin folder:

```text
# Carbon
<server>/carbon/plugins/TakaroConnector.cs

# Oxide / uMod
<server>/oxide/plugins/TakaroConnector.cs
```

For example `/home/steam/rust/carbon/plugins/TakaroConnector.cs` on Linux or
`C:\RustServer\oxide\plugins\TakaroConnector.cs` on Windows. The framework compiles and loads it
right away; no restart is needed.

### 4. Configure

On its first load the plugin creates its config file:

```text
# Carbon
<server>/carbon/configs/TakaroConnector.json

# Oxide / uMod
<server>/oxide/config/TakaroConnector.json
```

Paste the registration token into `RegistrationToken` and save. The plugin re-reads the file every
5 seconds and connects; no restart and no plugin reload.

```json
{
  "RegistrationToken": "your-registration-token-here",
  "IdentityToken": "3f2b0c1e-...",
  "WebSocketUrl": "wss://connect.takaro.io/",
  "Debug": false,
  "ServerChatName": ""
}
```

| Field | What it is |
|---|---|
| `RegistrationToken` | Your Takaro registration token. Required. |
| `IdentityToken` | This server's identity in Takaro, generated on first load. Do not change it, or Takaro sees a new server. |
| `WebSocketUrl` | Leave as is. |
| `Debug` | `true` logs every message sent and received. |
| `ServerChatName` | Optional sender name on messages from Takaro (see below). |

Environment variables `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_WS_URL`, `TAKARO_DEBUG` and
`TAKARO_SERVER_CHAT_NAME` win over the file, field by field; changing them needs a server restart.

**Server chat name (optional).** Messages from Takaro show as `Name: message`, in a broadcast and in a whisper. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `ServerChatName` instead (a file from an older plugin version has no such key, so add it; applies within 5 seconds); the name set in Takaro wins over it. With neither, only the text shows.

### 5. Check that it works

The server console shows lines prefixed with `[Takaro]`:

```text
[Takaro] Config: <server>/carbon/configs/TakaroConnector.json (registration token set)
[Takaro] Connecting to wss://connect.takaro.io/
Loaded plugin TakaroConnector v<version> by Takaro [1667ms]
[Takaro] WebSocket connected
[Takaro] Identified successfully, server ID: <id> (protocol 1)
```

and the game server shows as online in Takaro. A missing token shows a banner naming the file to
fix:

```text
*************************************************************************
  RegistrationToken not set, the server is not connected to Takaro.
  Paste the registration token from Takaro into <server>/carbon/configs/TakaroConnector.json
  and save it. The plugin connects within a few seconds, no restart needed.
*************************************************************************
```

A wrong token shows `Takaro rejected identify: Invalid registrationToken provided.` Save a
corrected token and it reconnects within a few seconds.

### 6. Upgrade

Replace `TakaroConnector.cs` in the plugin folder. Carbon and Oxide reload the plugin by
themselves; no restart is needed. Keep `TakaroConnector.json`: the server keeps its token and
identity. Coming from `rust-v0.1.0` or older: those releases made every timed ban permanent, so
upgrade to get working timed bans.

## What works, what doesn't

Checked on Rust build 26139947 with Carbon v2.0.262, 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects on its own after a server restart. |
| Player list | ✅ | Steam id, IP and ping; sleeping players can be looked up. |
| Player location | ✅ | |
| Teleport | ✅ | To the exact coordinates with no ground snapping; pick a safe height. |
| Player inventory | ⚠️ | Main inventory, hotbar and worn items; item quality is always empty. |
| Item list | ✅ | Every item with its display name, for example "Assault Rifle". |
| Entity list | ✅ | Animals and NPCs with display names. |
| Locations | ⚠️ | Lists the map's monuments. Not checked. |
| World map | ⚠️ | The server draws the game's own world map once per map. Not checked on takaro.io yet. |
| Send a message | ✅ | Broadcasts and whispers, after the server chat name when one is set. |
| Console commands | ⚠️ | Returns the output and Rust's errors, but an unknown command comes back as a success. |
| Kick | ✅ | The reason is shown to the player. |
| Ban and unban | ✅ | Timed bans end on time; permanent bans go into the server's own ban list. |
| Ban list | ✅ | Timed bans with their end time. |
| Give an item | ✅ | Into the inventory, or dropped at the player's feet when it is full. |
| Shop | ✅ | Buying a listing delivers it into the inventory; balances and kill rewards. |
| Shut down the server | ✅ | Saves and quits two seconds after answering Takaro. |
| Player joined / left | ✅ | Left is also sent on kicks and bans. |
| Chat messages | ✅ | With the chat channel. |
| Player death | ✅ | With the position; the killer only when it was another player. |
| Entity killed | ✅ | Animals and NPCs killed by a player, with the weapon. |
| Server log lines | ⚠️ | Console lines are sent, minus Carbon's and the plugin's own; Takaro does not keep them. |
| Discord chat bridge | ✅ | Both ways. |
| Modules | ✅ | Chat commands, welcome and scheduled messages. |
| Game name, version and icon | ✅ | Takaro shows Rust with its icon and the running game build. Not checked on takaro.io yet. |
| Oxide/uMod | ⚠️ | Written for the Oxide plugin API, but only run on Carbon. |

## Known issues

- The first start on a new map pauses the server while it draws the world map (a few seconds on a
  small map, longer on big ones). Later starts reuse the drawing.
- Inventory items always come back with an empty quality.
- Unknown console commands look successful; Rust gives no error for them.
- Only Rust build 26139947 with Carbon v2.0.262 is checked; other builds will likely load but are
  not checked.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
