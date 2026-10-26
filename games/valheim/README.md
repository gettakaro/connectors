# Takaro Valheim Connector

A BepInEx plugin that runs inside a Valheim dedicated server and connects it to Takaro. Players
do not install anything: they join with plain, unmodified Valheim. Only reading a player's
inventory needs an optional mod on that player's game
([step 7](#7-optional-inventory-mod-for-players)). Built against **Valheim 1.0.20** with
**BepInExPack Valheim 5.4.2351**.

## Install

### 1. Before you start

You need:

- A **Valheim dedicated server** that you can stop, start and copy files to. Releases are built
  and tested for the Linux dedicated server; Windows servers are not tested.
- **BepInExPack Valheim 5.4.2351** (`denikson/BepInExPack_Valheim` on Thunderstore) installed on
  that server. Start the server once after installing it; `BepInEx/plugins/` must exist.
- A Takaro game server of type **Generic** and its **registration token**.
- Outbound access to `wss://connect.takaro.io/`. Nothing needs to be port-forwarded.

The exact server builds this connector is maintained for:

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `linux-1.0.20` | 1.0.20 | linux | — | None | candidate | contract |
<!-- takaro-maint:targets:end -->

### 2. Download

Download **`takaro-valheim-plugin.zip`** from the downloads page,
<https://takaro.io/connectors/valheim>, or from the latest `valheim-v4.x.x` release at
<https://github.com/gettakaro/connectors/releases>. The release also has the same zip under its
target name, for example `takaro-valheim-plugin-linux-1.0.20-4.3.0.zip`, and a `SHA256SUMS`
file to check the download with. Do not use the `valheim-dev` pre-release.

These steps are for 4.3.0 and later (4.2.x kept its settings in a `takaro.cfg` inside the plugin
folder). Upgrading from 4.2.x or older: see [UPGRADING.md](UPGRADING.md).

### 3. Copy it into place

Stop the server. Unzip so the `TakaroValheim` folder lands directly in `BepInEx/plugins/`:

```
<server>/BepInEx/plugins/TakaroValheim/
    TakaroValheim.dll
    Takaro.Valheim.Core.dll
    ...the other DLLs from the zip
    manifest.json
    README.txt
```

Copy the whole folder as-is.

### 4. Configure

The config is one file, `BepInEx/config/com.takaro.valheim.cfg`. Start the server: the connector
creates the file, with a new `identityToken` for this server and a `serverName` made of the
server's own name and the start of that identity. Open it, paste your Takaro registration token
after `registrationToken =` and save. Saving is enough: the connector checks the file every few
seconds and connects, no restart needed.

This is the file with its defaults. When the connector creates it, `identityToken` and
`serverName` are already filled in. You can also write it yourself before the first start;
the connector then only adds an `identityToken`.

```ini
## Takaro Valheim connector settings. Saving this file applies the token, identity,
## serverName and takaroWsUrl within a few seconds; other settings need a server restart.

[Takaro]

## Takaro registration token: paste it here and save; the connector connects within a few seconds.
registrationToken = your-registration-token-here

## The server name shown in Takaro; must be unique in your Takaro domain. Empty: the server's own name plus the start of identityToken.
serverName =

## Identifies this server in Takaro. Filled in by the connector; keep it.
identityToken =

## Takaro connector WebSocket URL; keep the default.
takaroWsUrl = wss://connect.takaro.io/

## Connector log level.
logLevel = Information

## Forward connector log events to Takaro.
enableLogEvents = true

## Semicolon-separated exact console commands allowed for executeConsoleCommand.
commandAllowlistExact = help

## Semicolon-separated console command prefixes allowed for executeConsoleCommand.
commandAllowlistPrefixes =

## Name shown in game chat for Takaro messages (unless Takaro sends its own sender name).
chatSenderName = Takaro
```

`registrationToken`, `serverName`, `identityToken` and `takaroWsUrl` apply as soon as you save;
the other keys apply after a server restart. The connector only fills in an empty
`identityToken` and never changes anything else in the file. Emptying `registrationToken`
disconnects the server. `chatSenderName` is at most 128 characters.

### 5. Check that it worked

In the server console (or `BepInEx/LogOutput.log`), look for:

```
Takaro Valheim identified as gameServerId=<your game server id> (protocol version 1).
```

The game server then shows as **online** in Takaro. When the first player joins, the log also
shows:

```
Takaro Valheim chat participant 'Takaro' active (server-side chat relay and kill witness).
```

If no token is set yet, the log shows a banner instead:

```
*************************************************************************
  registrationToken not set, the server is not connected to Takaro.
  Paste the registration token from Takaro into <server>/BepInEx/config/com.takaro.valheim.cfg
  and save it. The connector connects within a few seconds, no restart needed.
*************************************************************************
```

If the token is wrong, the banner starts with `Takaro rejected this server:`; if another game
server in your Takaro domain has the same name, it starts with `Takaro refused the server name`.
Fix the value in `com.takaro.valheim.cfg` and save; the connector reconnects within a few seconds.

### 6. Upgrading

Stop the server, delete `BepInEx/plugins/TakaroValheim/` and unzip the new version in its place.
Delete `BepInEx/cache/chainloader_typeloader.dat` before starting again, or BepInEx may keep
loading the old plugin. Your config in `BepInEx/config/com.takaro.valheim.cfg` is not touched,
so the server keeps its token and its identity in Takaro.

Coming from 4.2.x? Do not delete the folder: unzip the new version over it, so the connector can
carry your `takaro.cfg` settings over. Details for 4.2.x and older: [UPGRADING.md](UPGRADING.md).

### 7. Optional: inventory mod for players

Everything except **Player inventory** works without it. A player who wants Takaro to see their
inventory installs it on their own game; nobody else needs it, and players without it play
normally on the same server.

1. On the player's PC, install **BepInExPack Valheim 5.4.2351** into the Valheim game folder
   (`steamapps/common/Valheim`) and start the game once.
2. From the same release, download **`takaro-valheim-inventory-companion.zip`** and unzip it so
   the `TakaroValheimInventoryCompanion` folder lands in `BepInEx/plugins/` of that game folder.
3. Start Valheim and join the server.

It holds no Takaro token and only talks to the Valheim server the player is connected to. In the
server's `BepInEx/LogOutput.log` a working mod shows
`Takaro Valheim inventory companion negotiated with peer ...`. A mod from another version is
logged as ignored; the player stays connected.

## What works, what doesn't

Tested on 2026-10-22 against **Valheim 1.0.20** (Steam build 25909431) with BepInExPack 5.4.2351,
connector **4.3.0** (release candidate), Takaro's staging service and an unmodified Valheim
1.0.20 game client. Rows marked "not re-tested" were not checked in that run.
✅ = works, ⚠️ = works with a caveat or not re-tested, ❌ = unsupported.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | Dials out to Takaro; server shows online. |
| Game name, build and icon | ✅ | Takaro shows "Valheim", the server's game build (1.0.20), the Valheim icon and the connector version. On a fresh install the icon can take up to about two hours to appear (Takaro fetches it on its periodic sync when the first try came before the world loaded). |
| Player list | ✅ | Name, Steam id and online state. |
| Single player lookup | ❌ | Not offered to Takaro; Takaro uses the player list. |
| Player location | ✅ | The server's known position; never invented. |
| Player inventory | ⚠️ | Only for players with the optional inventory mod; not re-tested on 1.0.20. Without the mod Takaro gets no inventory. |
| Give an item | ✅ | Dropped at the player's feet; picked up automatically. |
| Item catalogue | ✅ | English in-game names (`Raspberries`, `Crude Bow`), filled when Takaro syncs. |
| Entity catalogue | ✅ | English in-game names (`Boar`, `Hen`), filled when Takaro syncs. |
| Locations / points of interest | ⚠️ | The Sacrificial Stones, boss altars, Haldor, Hildir and the Bog Witch, with their English names, on the map after Resync Locations. Proven on Takaro staging; not yet verified on takaro.io. |
| Run a console command | ⚠️ | Only allowlisted commands run; the default allows just `help`. Others are refused. |
| Broadcast a message | ✅ | Shown in normal chat with the sender name, or the name Takaro sends. |
| Whisper a player | ✅ | Shown in that player's normal chat. |
| Teleport a player | ✅ | Moves the player to the requested position. |
| Kick a player | ✅ | Player sees "kicked"; the server stays up. |
| Ban a player | ⚠️ | Player is banned and dropped, and a rejoin is refused; the reason is discarded. |
| Unban a player | ✅ | Removed from Valheim's ban list; the player can rejoin. |
| Ban list | ✅ | Matches Valheim's own ban list. |
| Shut the server down | ✅ | Answers Takaro, then shuts down cleanly. |
| Player joined event | ✅ | Arrives once the player has spawned. |
| Player left event | ✅ | Arrives on log out and on kick. |
| Player chat event | ✅ | Works even when a player is alone. |
| Player death event | ✅ | Player and position included. |
| Entity kill event | ✅ | Creature and weapon included. Counts the kills Valheim itself credits to the player, so a creature the player hit but another creature finished also counts. |
| Log events | ⚠️ | Sent while `enableLogEvents` is on; Takaro does not keep them as events. |
| World map | ⚠️ | The whole world, drawn by the server from the world seed; ready about 4 minutes after the first start, then at once on later starts. Works on Takaro's staging service; not yet verified on takaro.io. |
| Modules: chat commands | ✅ | `@` commands work; replies show in normal chat. |
| Modules: hooks | ✅ | Join, leave and chat hooks fire. |
| Modules: cronjobs | ✅ | Run on schedule for this server. |
| Modules: teleports | ✅ | `@settp` and `@tp` move the player. |
| Discord: game chat → Discord | ⚠️ | Takaro posts chat, joins and leaves, and Discord accepts each post; not checked in the Discord app. |
| Discord: Discord → game chat | ✅ | A Discord message shows in game chat as `[D] <name>: <text>`. |
| Shop & economy | ✅ | `@balance` works; chat purchases deliver at the buyer's feet. |
| Reconnects after a server restart | ✅ | Re-identifies on its own; events resume. |
| Events raised while connecting | ✅ | Held until Takaro accepts the server, then sent once. |
| Config change without a restart | ✅ | Saving `com.takaro.valheim.cfg` reconnects within a few seconds. |
| Takaro outage | ✅ | Events are kept and delivered once after reconnecting. |

### Known issues

- Players see a "Takaro" entry in the in-game player list; it is how server chat works.
- Without the mod, a kill finished by a creature after the player hit it still counts for the player.
- Ban reasons are discarded: Valheim's ban list stores only the player id.
- Given and shop items drop at the player's feet; anyone nearby could grab them first.
- A shop order placed while Takaro has lost its connection to the server is not delivered, and
  Takaro still takes the coins.
- Console commands only run when allowlisted; the default allowlist is just `help`.

---

Upgrading from an older version: see [UPGRADING.md](UPGRADING.md).
Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
