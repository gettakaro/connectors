# Takaro Minecraft Connector

A server-side-only connector (version **0.1.0**) that connects a Minecraft dedicated server to
Takaro. It ships for **Fabric**, **Paper** and **NeoForge**; players do not install anything.
The results below were proven on **Fabric / Minecraft 26.2 / Java 25**.

## Install

Download the latest release: https://takaro.io/connectors/minecraft

### 1. Before you start

You need:

- A **Minecraft dedicated server** you can stop, start and copy files to, running one of:
  - **Fabric** on **Minecraft 26.2** with **Java 25** and the **Fabric API** mod installed
    (the connector requires Fabric Loader 0.19.5 or newer).
  - **Paper** on **Minecraft 1.21.11** with **Java 21**.
  - **NeoForge** on **Minecraft 1.21.11** with **Java 21**.
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

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

Players' clients must match the server's Minecraft version. A 26.2 client cannot join a 1.21.11
server, and a 1.21.11 client cannot join a 26.2 server.

### 2. Download the connector

Download the jar for your platform from the latest `minecraft-vX.Y.Z` release:

> https://github.com/gettakaro/connectors/releases

| Your server | File to download |
|---|---|
| Fabric | `takaro-minecraft-mod-fabric-26.2-<version>.jar` |
| Paper | `takaro-minecraft-mod-paper-1.21.11-<version>.jar` |
| NeoForge | `takaro-minecraft-mod-neoforge-1.21.11-<version>.jar` |

Every jar is named after the server build it was made for. Releases before the next
`minecraft-v*` used `takaro-fabric-<version>.jar`, `takaro-paper-<version>.jar` and
`takaro-neoforge-<version>.jar`. Those names keep working: for two stable releases each one
also ships as a byte-identical copy of that platform's default target (Fabric 26.2,
Paper 1.21.11, NeoForge 1.21.11), so existing links and scripts do not have to change yet.

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/minecraft-v<version>/<file>`

Use `minecraft-v0.1.0` or newer. Do not use the `minecraft-dev` pre-release; that is an untested
rolling build.

### 3. Copy it into place

Stop the server, then put the single jar into the right folder for your platform:

```
Fabric     <server>/mods/takaro-minecraft-mod-fabric-26.2-<version>.jar
NeoForge   <server>/mods/takaro-minecraft-mod-neoforge-1.21.11-<version>.jar
Paper      <server>/plugins/takaro-minecraft-mod-paper-1.21.11-<version>.jar
```

That is the whole install — one file, no extra libraries. On Fabric, keep the Fabric API jar in
`mods/` next to it.

### 4. Configure

Start the server once and let it finish loading, then stop it again. The connector writes an empty
config file on that first start:

| Your server | Config file |
|---|---|
| Fabric | `<server>/config/takaro.json` |
| NeoForge | `<server>/config/takaro.properties` |
| Paper | `<server>/plugins/TakaroMinecraft/config.yml` |

Open it and fill in the WebSocket URL, your Takaro **registration token**, and an **identity
token** — any string you choose that is unique to this server (for example `my-smp-survival`).
The connector does not invent one for you, and the server will not connect while the URL is empty.

**Fabric** (`config/takaro.json`):

```json
{
  "websocket": { "url": "wss://connect.takaro.io/" },
  "authentication": {
    "identity_token": "my-smp-survival",
    "registration_token": "your-registration-token-here"
  }
}
```

**NeoForge** (`config/takaro.properties`):

```properties
takaro.websocket.url=wss://connect.takaro.io/
takaro.authentication.identity_token=my-smp-survival
takaro.authentication.registration_token=your-registration-token-here
```

**Paper** (`plugins/TakaroMinecraft/config.yml`):

```yaml
takaro:
  websocket:
    url: "wss://connect.takaro.io/"
  authentication:
    identity_token: "my-smp-survival"
    registration_token: "your-registration-token-here"
```

Leave the `reconnect` values alone. Save the file and start the server.

### 5. Check that it worked

In the server console / `logs/latest.log`, under the `Takaro` logger:

```
Connecting to Takaro at wss://connect.takaro.io/
Identified successfully, server ID: <your server id>
```

And in Takaro, the game server shows as **online**. If it stays offline, look for
`Identify failed:` or `No WebSocket URL configured` in the log — the registration token and the
`url` line in the config file are the first things to re-check.

**Version check.** Just before connecting, the connector logs one `Takaro target-check:` line
saying which server build it was made for and which one it found. If they do not match it refuses
to connect and says why, rather than half-working on a server it was never tested on:

```
Takaro refuses to connect: target fabric-26.2 does not match this server
(built for game version 26.2, this server runs 26.1.2).
Set TAKARO_TARGET_POLICY=warn to override.
```

Download the jar for your server's version instead. `TAKARO_TARGET_POLICY=warn` connects anyway
and `off` skips the check entirely, but neither makes an untested combination work.

### 6. Upgrading

**Stop the server first.** Delete the old `takaro-<platform>-<version>.jar` from `mods/`
(or `plugins/`) and drop the new jar in its place, then start the server again. Leave the config
file alone — your tokens survive the upgrade. Never swap the jar under a running server.

## What works, what doesn't

Verified end to end on **2026-09-14** against a real **Fabric** server (Minecraft **26.2**,
Java 25) with a real game client connected. **Paper and NeoForge have not been tested at this
level — treat every row below as unverified on those two platforms.**
✅ = works, ⚠️ = works with a caveat, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | Connects, identifies and stays up; reconnects by itself with backoff after outages. |
| Server restart / reconnect | ✅ | Comes back on its own after a restart, and players online at shutdown are correctly reported as offline. |
| Player list | ⚠️ | The connector answers the player-list request, but the live test never exercised it on its own — player state in Takaro was proven through join/leave events instead. |
| Single player lookup | ✅ | Name, platform id and online state match the game. |
| Player location | ✅ | Matches the server's own position readout (rounded to whole blocks). |
| Player inventory | ✅ | Matches what the player is carrying in game, and changes are reported as they happen. |
| Item catalogue | ✅ | 1,536 items synced. |
| Entity catalogue | ✅ | 158 entities synced. |
| Locations / points of interest | ❌ | Not implemented — the connector always answers with an empty list. Minecraft has no fixed list of named locations. |
| Chat messages from players | ✅ | Real player chat reaches Takaro with the player attached. |
| Broadcast a message | ✅ | Shown to everyone in the server chat. |
| Whisper a player | ✅ | Reaches the intended player. Only one client was connected, so "nobody else sees it" has not been confirmed. |
| Give an item | ⚠️ | The item lands straight in the player's inventory. Item quality / durability is ignored — every item arrives in default condition. |
| Teleport a player | ⚠️ | Works; the height (Y) is snapped to the ground, so the player lands on solid ground rather than at the exact Y you asked for. |
| Run a console command | ⚠️ | The command runs and success/failure comes back, but the command's own output text is not returned — it is always empty. |
| Kick a player | ✅ | The player is dropped from the server with the reason shown. |
| Ban a player (timed and permanent) | ✅ | The ban lands on the game server and the player is refused on rejoin. |
| Unban a player | ✅ | Clears the ban on the game server. |
| Ban list | ✅ | Matches the server's own ban list. |
| Shut the server down | ✅ | The server stops cleanly on request (all chunks saved). Your host has to start it again. |
| Player joined event | ✅ | Arrives in Takaro within a second of the join. |
| Player left event | ✅ | Also fired for everyone still online when the server itself shuts down. |
| Player chat event | ✅ | See "Chat messages from players". |
| Player death event | ✅ | Reaches Takaro and can drive modules. |
| Entity kill event | ✅ | Proven with a real kill (zombie, diamond sword); the entity type and the weapon used are included. |
| Log events | ❌ | The connector never sends server log lines to Takaro. |
| Map info | ❌ | Not implemented — the connector answers "action not implemented". |
| Map tiles | ❌ | Not supported by Takaro for this connector type yet; nothing on the game server side changes that. |
| Discord chat bridge | ⚠️ | Game → Discord works: chat, deaths and cron messages were all delivered to the Discord channel. Discord → game was **not** confirmed with a post from a real Discord account. Use the `chatBridgeNoEcho` module; the plain chat bridge echoes Takaro's own messages back into the game. |
| Shop & economy | ✅ | Buying in chat (`@shop`), ordering through the Takaro API, currency grants and balance checks all work, and purchases arrive in the inventory. Buying without enough currency is correctly refused. |

### Known issues

- **Discord → game is unconfirmed.** Messages from Discord reaching in-game chat were never
  proven with a post from a real Discord account.
- **Item quality is ignored.** Everything the shop or Takaro hands out arrives in default
  condition; there is no durability or quality control.
- **Teleport snaps to the ground.** The Y you ask for is replaced by ground level at that spot.
- **No map and no locations.** Takaro's API does not support map tiles for Generic-connector
  servers, and Minecraft has no named-location list for the connector to report.
- **Paper and NeoForge are untested.** They build and ship, but no end-to-end run has been done
  on either; only Fabric on Minecraft 26.2 has been proven.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
