# Takaro Rust Connector

A server-side-only plugin (version **0.0.3**) that connects a Rust dedicated server to Takaro. It is
written for the **Oxide/uMod** plugin API (`Oxide.Plugins` / `RustPlugin`) and is developed and
tested on **Carbon**, which runs the same plugins. Players do not install anything.

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

There is **no tagged `rust-v*` release yet**. Take the plugin from one of these two places:

- The rolling pre-release: download **`TakaroConnector.cs`** from
  <https://github.com/gettakaro/connectors/releases/tag/rust-dev> — this is an untested build,
  rebuilt on every push to `main`.
- Or copy the file straight out of the repository: `games/rust/mod/TakaroConnector.cs`.

Both are the same single C# source file. There is nothing to compile — Carbon and Oxide compile
`.cs` plugins at runtime.

### 3. Copy it into place

Copy the file into your framework's plugin folder:

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
[Takaro] Connecting to wss://connect.takaro.io/
[Takaro] WebSocket connected
[Takaro] Received server hello, sending identify...
[Takaro] Identified and connected, server ID: <id>
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

**Nothing in this table has been proven in a live test.** There is no recorded hard-test evidence
for the Rust connector. The statuses below come from reading the plugin source
(`mod/TakaroConnector.cs`, version 0.0.3) only: ⚠️ means the plugin implements it but no live test
has confirmed it, ❌ means it is not implemented or not supported.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ⚠️ | Connects outbound over WebSocket and answers Takaro's reachability check. Implemented, not verified in a live test. |
| Server restart / reconnect | ⚠️ | Reconnects on its own with exponential backoff (5 s up to 5 min). Implemented, not verified in a live test. |
| Player list | ⚠️ | Returns name, Steam id, IP and ping for every connected player. Implemented, not verified in a live test. |
| Single player lookup | ⚠️ | Finds connected and sleeping players by Steam id. Implemented, not verified in a live test. |
| Player location | ⚠️ | Returns the player's position, falling back to the last known position when they are offline. Implemented, not verified in a live test. |
| Player inventory | ⚠️ | Main inventory, hotbar and worn items. Item quality is always empty. Implemented, not verified in a live test. |
| Item catalogue | ⚠️ | Every item definition the server knows, by shortname. Implemented, not verified in a live test. |
| Entity catalogue | ⚠️ | Built from the server's prefab manifest, with corpses and ragdolls filtered out. Implemented, not verified in a live test. |
| Locations / points of interest | ⚠️ | Returns the map's monuments. Implemented, not verified in a live test. |
| Chat messages from players | ⚠️ | Player chat is forwarded with the player and the chat channel attached. Implemented, not verified in a live test. |
| Broadcast a message | ⚠️ | Sent to everyone in the server chat. Implemented, not verified in a live test. |
| Whisper a player | ⚠️ | Sent to the named player's chat only. Implemented, not verified in a live test. |
| Give an item | ⚠️ | Goes into the player's inventory; if there is no room it drops at their feet. Implemented, not verified in a live test. |
| Teleport a player | ⚠️ | Moves the player to the exact coordinates given, with no ground snapping. Implemented, not verified in a live test. |
| Run a console command | ⚠️ | Runs as a server console command and returns the output or the error. Implemented, not verified in a live test. |
| Kick | ⚠️ | Drops the player with the reason shown. Implemented, not verified in a live test. |
| Ban (timed and permanent) | ⚠️ | Timed bans are enforced by the plugin's own ban record (not verified in a live test). Permanent bans go into the server's own ban list; either way the player is kicked. |
| Unban | ⚠️ | Clears both the plugin's ban record and the server's ban list. Implemented, not verified in a live test. |
| Ban list | ⚠️ | Returns the server's banned users (no expiry) plus the plugin's timed bans with their real expiry. Not verified in a live test. |
| Shut the server down | ⚠️ | Runs the server's `quit` command. Implemented, not verified in a live test. |
| Player joined event | ⚠️ | Sent when a player connects. Implemented, not verified in a live test. |
| Player left event | ⚠️ | Sent when a player disconnects. Implemented, not verified in a live test. |
| Player chat event | ⚠️ | See "Chat messages from players". Implemented, not verified in a live test. |
| Player death event | ⚠️ | Sent when a player dies. Implemented, not verified in a live test. |
| Entity kill event | ⚠️ | Sent when an entity is killed, with the weapon used where it can be read. Implemented, not verified in a live test. |
| Log events | ⚠️ | Server console lines are forwarded, minus Carbon's and the plugin's own. Takaro does not store server log lines as events, so they cannot be searched or used in modules. |
| Map info | ❌ | The plugin does not implement it. |
| Map tiles | ❌ | Not supported by Takaro for Generic-connector servers. |
| Discord chat bridge | ⚠️ | Nothing in the plugin blocks it — chat in and chat out are both implemented — but the bridge has never been tried on Rust in either direction. |
| Shop & economy | ⚠️ | Rests on give-item, chat commands and console commands, which are all implemented; no shop purchase has been run on Rust. |

### Known issues

- **Nothing here is live-tested.** Treat every row above as "should work", not "does work". If you
  run this on a real server, expect to find things.
- **No map.** Takaro's API does not support map tiles for Generic-connector servers, and the plugin
  does not answer map-info requests either.
- **Item quality is not reported.** Inventory entries always come back with an empty quality field.
- **No tagged release.** The only published build is the rolling `rust-dev` pre-release, which is
  rebuilt on every push to `main` and is not meant for production.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
