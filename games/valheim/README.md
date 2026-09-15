# Takaro Valheim Connector

A server-side-only BepInEx plugin (version **3.0.1**) that connects a Valheim dedicated
server to Takaro. It is the only half that holds your Takaro token, it runs inside the
dedicated-server process, and players install nothing. Client-side Takaro mods are
rejected by rule for this connector, so everything below is what the plugin does on its
own. Last live-tested against a **Valheim 1.0.7** dedicated server.

## Install

### 1. Before you start

You need:

- A **Valheim dedicated server** (Linux or Windows) that you can stop, start and copy
  files to.
- **BepInExPack Valheim** (the `denikson/BepInExPack_Valheim` package from Thunderstore)
  already installed on that server. The plugin is a BepInEx **5** plugin built for .NET
  Framework 4.7.2 — the pack ships exactly that loader. Install it and start the server
  once before going further; if `BepInEx/plugins/` does not exist yet, BepInEx is not
  installed.
- A **Takaro account** with a game server created of type **Generic**, and its
  **registration token** (Takaro shows it when you create the game server).
- Outbound network access to `wss://connect.takaro.io/`. The plugin dials out; nothing
  needs to be port-forwarded to it.

### 2. Download the plugin

Download **`takaro-valheim-plugin.zip`** from the latest `valheim-vX.Y.Z` release on the
releases page:

> https://github.com/gettakaro/connectors/releases

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/valheim-v<version>/takaro-valheim-plugin.zip`

Use `valheim-v3.0.1` or newer. The same release also carries
`takaro-valheim-companion.zip`; that one is a **client** package and is not part of this
install — do not copy it onto the server. Do not use the `valheim-dev` pre-release either,
that is an untested rolling build.

The zip contains a single folder, `TakaroValheim/`. That whole folder is the plugin.

### 3. Copy it into place

Stop the server, then unzip so that the `TakaroValheim` folder ends up directly inside
`BepInEx/plugins/`:

```
<server>/BepInEx/plugins/TakaroValheim/
    TakaroValheim.dll
    Takaro.Valheim.Core.dll
    Takaro.Valheim.Companion.Protocol.dll
    Microsoft.Bcl.AsyncInterfaces.dll
    System.Buffers.dll
    System.Memory.dll
    System.Numerics.Vectors.dll
    System.Runtime.CompilerServices.Unsafe.dll
    System.Text.Encodings.Web.dll
    System.Text.Json.dll
    System.Threading.Tasks.Extensions.dll
    System.ValueTuple.dll
    manifest.json
    README.txt
```

All of those files are needed — copy the folder as-is, do not cherry-pick the DLLs.

Examples:

- Linux: `/home/steam/valheim-dedicated-server/BepInEx/plugins/TakaroValheim/`
- Windows: `C:\ValheimServer\BepInEx\plugins\TakaroValheim\`

### 4. Configure

Start the server once and let it finish loading, then stop it again. The plugin creates
its config at:

```
<server>/BepInEx/config/com.takaro.valheim.cfg
```

Open that file and set two things under the `[Takaro]` section:

```ini
[Takaro]

## Takaro registration token.
registrationToken = your-registration-token-here

## Client companion policy: disabled, optional, or required.
companionMode = disabled
```

`companionMode` defaults to `disabled`; only change it if you deploy the client companion.

Leave `takaroWsUrl` as it is, and leave `identityToken` alone — the plugin fills it in by
itself after the first successful registration. Save the file. Restart the dedicated server
so the saved configuration is loaded.

### 5. Check that it worked

In `BepInEx/LogOutput.log` on the server:

```
[Info   :   BepInEx] Loading [Takaro Valheim 3.0.1]
[Info   :Takaro Valheim] Takaro Valheim connector started.
[Info   :Takaro Valheim] Takaro Valheim WebSocket connected.
[Info   :Takaro Valheim] Takaro Valheim identified as gameServerId=<your game server id>.
```

The `identified as gameServerId=` line is the one that matters — it means Takaro accepted
your registration token. And in Takaro, the game server shows as **online**. If it stays
offline, `registrationToken` in the config file is the first thing to re-check.

### 6. Upgrading

**Stop the server first.** Delete `<server>/BepInEx/plugins/TakaroValheim/` and unzip the
new version in its place. Then **delete `<server>/BepInEx/cache/chainloader_typeloader.dat`**
before starting the server again — the release archives use fixed timestamps, so BepInEx
can otherwise keep cached metadata from the previous DLL of the same size and load the old
plugin. Leave `BepInEx/config/com.takaro.valheim.cfg` alone; your token and identity
survive the upgrade.

## What works, what doesn't

Status for the dedicated-server plugin **on its own**, with ordinary unmodified Valheim
clients. The most recent live run was **2026-09-02** against a real dedicated server with
a real game client attached.
✅ = works, ⚠️ = works with a caveat or is unproven, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | Dials out to Takaro and stays identified; the server shows online in Takaro. |
| Server restart / reconnect | ✅ | After a dedicated-server restart the plugin re-identifies with no config change and events resume. |
| Player list | ✅ | Name, Steam id and online state. This is how Takaro loads players for Valheim. |
| Single player lookup | ⚠️ | Implemented, but Takaro exposes no route that asks for one player at a time, so it has never been proven end to end. |
| Player location | ✅ | The server's own known position for the player; a position is never invented. A disconnected player keeps a real last-known position for 30 seconds. |
| Player inventory | ❌ | Valheim keeps a player's inventory on their own client — the dedicated server holds no inventory for a remote player, so there is nothing to read. The plugin returns an error rather than a fake empty bag. |
| Item catalogue | ✅ | 821 item prefabs synced. Takaro only asks for this when its sync job runs, so a freshly registered server shows an empty catalogue until then. |
| Entity catalogue | ✅ | 101 character prefabs synced, same sync-job timing as above. |
| Locations / points of interest | ❌ | The plugin finds them (11,293 in a live run), but Takaro's own route for this throws `NotImplementedError`, so they never reach you. |
| Chat messages from players | ❌ | Valheim sends normal chat between clients, not to the dedicated server. A live probe produced visible in-game chat and no server-side trace at all. |
| Broadcast a message | ❌ | Valheim gives a dedicated server no way to put text in a player's chat window; the plugin returns `companion_server_chat_unavailable`. |
| Whisper a player | ❌ | Same reason as broadcast. |
| Give an item | ⚠️ | The items **drop on the ground at the player's feet** — anyone nearby can pick them up, and they can be lost if the player is falling or swimming. |
| Teleport a player | ✅ | Uses Valheim's own teleport; proven live moving a player to exact coordinates. |
| Run a console command | ⚠️ | Only commands you allowlist run. The default allowlist is just `help`, so this does nothing useful until you add commands to `commandAllowlistExact` / `commandAllowlistPrefixes`. |
| Kick a player | ✅ | The player is dropped with Valheim's built-in kick and the reason is logged; the headless server stays up. |
| Ban a player (timed and permanent) | ⚠️ | The ban lands in Valheim's own ban list and the player is disconnected, but **the reason is thrown away** — Valheim's ban list stores one id per line and nothing else. Takaro reads the reason back as empty. |
| Unban a player | ✅ | Removes the id from Valheim's ban list; the ban list then reads back empty. |
| Ban list | ✅ | Matches Valheim's own ban entries. |
| Shut the server down | ✅ | Answers Takaro first, then Valheim shuts down cleanly and the process exits. |
| Player joined event | ✅ | Arrives in Takaro once the server has seen a real position for the player. |
| Player left event | ✅ | Arrives in Takaro after the player disconnects, carrying their real last-known position. |
| Player chat event | ❌ | Nothing to report — see "Chat messages from players". |
| Player death event | ❌ | The server sees a death packet go past but cannot tell whose character it is from its own state, so it deliberately emits nothing rather than guess. |
| Entity kill event | ❌ | Kills are resolved on the killing player's client; the dedicated server never sees who killed what. |
| Log events | ✅ | Connector log lines are forwarded to Takaro (`enableLogEvents`, on by default). |
| Map info | ❌ | Valheim's dedicated server has no map metadata to hand out. |
| Map tiles | ❌ | Takaro's API does not support map tiles for Generic-connector servers. |
| Discord chat bridge | ❌ | Never tested on Valheim, and it cannot work in either direction: player chat never reaches the server, and the server cannot write into a player's chat window. |
| Shop & economy | ⚠️ | Proven end to end: a player bought from the in-game shop, 100 currency was deducted and the goods arrived. But shop deliveries go through "Give an item", so **purchases land on the ground at the buyer's feet**, lootable by anyone nearby. |

Several ❌ rows above are ❌ only because Valheim keeps that information on the player's
own client. A separately published client package changes some of them; it is outside the
server-side-only install this README covers, and is documented in
[COMPANION.md](COMPANION.md).

### Known issues

- **No chat in either direction.** Valheim's dedicated server neither sees player chat nor
  can write into a player's chat window, so chat events, broadcasts, whispers and any
  Discord bridge are all off the table on a server-side-only install.
- **Shop purchases land on the ground.** They are lootable by anyone nearby and can be
  lost if the buyer is falling or swimming. Worth knowing before running an economy.
- **Bans lose their reason.** Valheim's ban list has nowhere to store one, so Takaro reads
  it back as empty.
- **Console commands are allowlisted and the allowlist starts almost empty.** Add what you
  need to `commandAllowlistExact` / `commandAllowlistPrefixes` or the action is useless.
  And when upgrading, delete `BepInEx/cache/chainloader_typeloader.dat` or BepInEx may keep
  loading the old plugin.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
