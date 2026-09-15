# Takaro 7D2D Mod

A server-side-only mod (version **0.1.4**) that connects a 7 Days to Die dedicated server to
Takaro. Tested against a **V 3.2.0 b10** dedicated server; players do not install anything.

## Install

### 1. Before you start

You need:

- A **7 Days to Die dedicated server** (Linux or Windows) that you can stop, start and copy files to.
- Access to the server's **`Mods/` folder** (create it if it does not exist).
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

### 2. Download the mod

Download **`takaro-7d2d-mod.zip`** from the latest `7d2d-vX.Y.Z` release on the releases page:

> https://github.com/gettakaro/connectors/releases

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/7d2d-v<version>/takaro-7d2d-mod.zip`

Use `7d2d-v0.1.4` or newer. The results in the table below were proven on the code that shipped in
0.1.4 (the dev build was labelled 0.1.6 during testing). Do not use the
`7d2d-dev` pre-release; that is an untested rolling build.

The zip contains a single folder, `Takaro/`. That whole folder is the mod.

### 3. Copy it into place

Stop the server, then unzip so that the `Takaro` folder ends up directly inside `Mods/`:

```
<server>/Mods/Takaro/
    ModInfo.xml
    Takaro.dll
    0Harmony.dll
    LiteDB.dll
    Mono.Cecil.dll
    Mono.Cecil.Mdb.dll
    Mono.Cecil.Pdb.dll
    Mono.Cecil.Rocks.dll
    MonoMod.Backports.dll
    MonoMod.Core.dll
    MonoMod.ILHelpers.dll
    MonoMod.Iced.dll
    MonoMod.RuntimeDetour.dll
    MonoMod.Utils.dll
    Newtonsoft.Json.dll
    System.Buffers.dll
    System.ValueTuple.dll
    com.rlabrecque.steamworks.net.dll
    websocket-sharp.dll
```

All of those files are needed — copy the folder as-is, do not cherry-pick the DLLs.

Examples:

- Linux: `/home/steam/7dtd/Mods/Takaro/`
- Windows: `C:\7DaysToDieServer\Mods\Takaro\`

### 4. Configure

Start the server once and let it finish loading, then stop it again. The mod creates its config at:

```
<server>/Takaro/Config.xml
```

Note this is **next to `Mods/`, not inside it** — it lands in the folder the server runs from
(for example `/home/steam/7dtd/Takaro/Config.xml`).

Open that file and paste your Takaro registration token:

```xml
<RegistrationToken>your-registration-token-here</RegistrationToken>
<Url>wss://connect.takaro.io/</Url>
```

Leave `<Url>` as it is, and leave `IdentityToken` alone — the mod fills it in by itself.
Save the file and start the server.

### 5. Check that it worked

In the server console / server log:

```
[MODS] Loaded Mod: Takaro (0.1.4)
```

In the mod's own log at `<server>/Takaro/logs/<M-D-YYYY>.log` (for example
`Takaro/logs/9-15-2026.log`):

```
Mod initialized successfully
WebSocket connection confirmed
```

And in Takaro, the game server shows as **online**. If it stays offline, the registration token in
`Config.xml` is the first thing to re-check.

### 6. Upgrading

**Stop the server first.** Delete `<server>/Mods/Takaro/` and unzip the new version in its place,
then start the server again. Leave `<server>/Takaro/Config.xml` alone — your token and identity
survive the upgrade. Never swap `Takaro.dll` under a running server; it can crash the server.

## What works, what doesn't

Verified end to end on **2026-09-15** against a real dedicated server (game build **V 3.2.0 b10**,
mod **0.1.4**) with a real game client connected.
✅ = works, ⚠️ = works with a caveat, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | Reconnects by itself after outages and server restarts; up to ~90 s of events right after an outage begins can be lost. |
| Server restart / reconnect | ✅ | The mod comes back on its own after a server restart, including with a player online. |
| Player list | ✅ | Name, platform id, IP and ping. This is how Takaro loads players for 7D2D. |
| Single player lookup | ⚠️ | The data is correct, but Takaro never asks for one player at a time on this game — it uses the player list instead. |
| Player location | ✅ | Polled about every 30 s; matches the server's own `lp` output. |
| Player inventory | ✅ | Matches what the player is carrying in game. |
| Item catalogue | ✅ | 26,329 items synced. |
| Entity catalogue | ✅ | 175 entities synced. |
| Chat messages from players | ✅ | Real player chat reaches Takaro with the player attached. |
| Broadcast a message | ✅ | Shown to everyone in the server chat. |
| Whisper a player | ⚠️ | The message reaches the intended player. Only one client was connected, so "nobody else sees it" has not been confirmed. |
| Give an item | ⚠️ | Works, but the item **drops as a bag at the player's feet** — the player has to walk over it and press E to collect. |
| Teleport a player | ⚠️ | Works; the height (Y) is snapped to the ground, so the player lands on solid ground rather than at the exact Y you asked for. |
| Run a console command | ✅ | Output and success/failure are returned, including the error text for unknown commands. |
| Kick a player | ✅ | The player is dropped from the server with the reason shown. |
| Ban a player (timed and permanent) | ⚠️ | The ban lands on the game server and the player is blocked. Takaro does not emit its own ban events for it, so the ban is only visible on the game side and in the ban list. |
| Unban a player | ⚠️ | Clears the ban on the game server; same missing Takaro ban events as above. |
| Ban list | ✅ | Matches the server's own ban list. |
| Shut the server down | ✅ | The server stops cleanly on request. |
| Player joined event | ✅ | Arrives in Takaro about 1 s after the join. |
| Player left event | ✅ | Arrives in Takaro shortly after the player disconnects. |
| Player chat event | ✅ | See "Chat messages from players". |
| Player death event | ✅ | Includes the position where the player died. |
| Entity kill event | ✅ | Proven with a real kill (zombie, Steel Club); the weapon used is included. |
| Log events | ⚠️ | The mod sends them, but Takaro does not store server log lines as events, so they cannot be searched or used in modules. |
| Map info | ⚠️ | The mod answers, but Takaro has no map view for this connector type yet. |
| Map tiles | ❌ | Not supported by Takaro for this connector type yet (the legacy native 7DTD integration has a map; this one does not). |
| Locations / points of interest | ❌ | The mod collects them (368 found), but Takaro has no way to ask for them yet. |
| Discord chat bridge | ⚠️ | Game → Discord works. Takaro records its own outgoing messages as chat, so the bridge echoes a message back and forth a few times before it stops (Takaro-side). Discord → game relayed from a **human** Discord account was never confirmed in the hard test. |
| Shop & economy | ✅ | Buying in game (`/shop`), ordering through the Takaro API, currency grants and balance checks all work. Purchases arrive as a bag at the player's feet. |

### Known issues

- **Chat bridge echoes.** Takaro stores its own outgoing messages as chat, so the Discord bridge
  forwards the server's own message and loops a few rounds before settling. This is on the Takaro
  side — the mod only reports chat from real players.
- **Locations are not reachable from Takaro.** The mod builds the catalogue, but there is no API
  route for it yet.
- **No map.** Takaro's API does not support map tiles for Generic-connector servers yet; nothing on
  the game server side changes that.
- **~90 s of events can be lost when a network outage starts.** The mod needs about that long to
  notice a dead connection; events sent into it in the meantime do not arrive. Everything after
  that is buffered and delivered once the connection is back.
- **Buffered events carry the time they were sent, not the time they happened.** After an outage,
  the delivered events are stamped with the moment they were flushed to Takaro.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
