# Takaro Valheim Connector

A BepInEx plugin (version **4.0.0**) that runs inside a Valheim dedicated server and connects it
to Takaro. Players do not install anything: they join with plain, unmodified Valheim. Only
reading a player's inventory needs an optional mod on that player's game
([step 7](#7-optional-inventory-mod-for-players)). Built against **Valheim 1.0.17** with
**BepInExPack Valheim 5.4.2351**.

## Install

### 1. Before you start

You need:

- A **Valheim dedicated server** (Linux or Windows) that you can stop, start and copy files to.
- **BepInExPack Valheim 5.4.2351** (`denikson/BepInExPack_Valheim` on Thunderstore) installed on
  that server. Start the server once after installing it; `BepInEx/plugins/` must exist.
- A Takaro game server of type **Generic** and its **registration token**.
- Outbound access to `wss://connect.takaro.io/`. Nothing needs to be port-forwarded.

The exact server builds this connector is maintained for:

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `linux-1.0.17` | 1.0.17 | linux | — | None | maintained | contract |
<!-- takaro-maint:targets:end -->

### 2. Download

From the latest `valheim-v4.x.x` release at <https://github.com/gettakaro/connectors/releases>
(also linked from <https://takaro.io/connectors/valheim>), download
**`takaro-valheim-plugin.zip`**. The same zip is also published under its target name, for
example `takaro-valheim-plugin-linux-1.0.17-4.0.0.zip`, next to a `SHA256SUMS` file. Do not use
the `valheim-dev` pre-release.

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
creates the file with a new `identityToken` for this server. Open it, paste your Takaro
registration token after `registrationToken =` and save. Saving is enough: the connector checks
the file every few seconds and connects, no restart needed.

You can also create the file yourself before the first start, with this content:

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

The connector writes a new `identityToken` into the file when it is empty, and never changes
anything else. `registrationToken`, `serverName`, `identityToken` and `takaroWsUrl` apply as soon
as you save; the other keys apply after a server restart. Emptying `registrationToken`
disconnects the server. `chatSenderName` is at most 128 characters.

### 5. Check that it worked

In the server console (or `BepInEx/LogOutput.log`), look for:

```
Takaro Valheim identified as gameServerId=<your game server id>.
Takaro Valheim chat participant 'Takaro' active (server-side chat relay).
```

The game server then shows as **online** in Takaro.

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
loading the old plugin. Replace only the plugin folder; your config at
`BepInEx/config/com.takaro.valheim.cfg` is never touched, so the server keeps its token and the
identity the connector wrote into that file on the first start.

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

### Upgrade from 3.x

Version 4.0.0 needs nothing on the game clients any more.

1. Replace the `TakaroValheim` folder as above.
2. Players remove the old client companion mod (`TakaroValheimCompanion`); it no longer works
   with this server. For inventory, install the new optional mod from step 7 instead.
3. The old `companionMode` config key is ignored and can be deleted.

## What works, what doesn't

Tested on Valheim 1.0.16 with BepInExPack 5.4.2351 and a vanilla game client, 2026-10-01.
✅ = works, ⚠️ = works with a caveat or not re-tested, ❌ = unsupported.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | Dials out to Takaro; server shows online. |
| Player list | ✅ | Name, Steam id and online state. |
| Single player lookup | ❌ | Takaro offers no route to ask for one player. |
| Player location | ✅ | The server's known position; never invented. |
| Player inventory | ⚠️ | Works only for players with the optional inventory mod. |
| Give an item | ✅ | Dropped at the player's feet; picked up automatically. |
| Item catalogue | ✅ | English in-game names, filled when Takaro syncs. |
| Entity catalogue | ✅ | English in-game names, filled when Takaro syncs. |
| Locations / points of interest | ❌ | Found by the plugin, but Takaro's route is not implemented. |
| Run a console command | ⚠️ | Only allowlisted commands run; default allows just `help`. |
| Broadcast a message | ✅ | Shown in normal chat with the sender name. |
| Whisper a player | ✅ | Shown in that player's normal chat only. |
| Teleport a player | ✅ | Moves the player to the requested position. |
| Kick a player | ✅ | Player is dropped; the server stays up. |
| Ban a player | ⚠️ | Player is banned and dropped; the reason is discarded. |
| Unban a player | ✅ | Removed from Valheim's ban list. |
| Ban list | ✅ | Matches Valheim's own ban list. |
| Shut the server down | ✅ | Answers Takaro, then shuts down cleanly. |
| Player joined event | ✅ | Arrives once the player has spawned. |
| Player left event | ✅ | Arrives with the last-known position. |
| Player chat event | ✅ | Works even when a player is alone. |
| Player death event | ✅ | Player and position included. |
| Entity kill event | ✅ | Every player kill counted; with the mod, assists aren't counted as kills. |
| Log events | ✅ | Connector log lines forwarded (`enableLogEvents`). |
| Map info | ✅ | The whole world map, drawn by the server from the world seed. |
| Map tiles | ✅ | Ready about 4 minutes after the first start, then at once on later starts. Shows the whole world, not only explored areas. |
| Modules: chat commands | ✅ | `@` commands work; replies show in normal chat. |
| Modules: hooks | ✅ | Join, leave and chat hooks fire. |
| Modules: cronjobs | ✅ | Run on schedule for this server. |
| Modules: teleports | ✅ | `@settp` and `@tp` move the player. |
| Discord: game chat → Discord | ⚠️ | Takaro posts chat, joins and leaves; Discord view not yet re-checked. |
| Discord: Discord → game chat | ⚠️ | Not re-tested in 4.0. |
| Shop & economy | ✅ | Chat purchases deliver at the buyer's feet. |
| Reconnects after a server restart | ✅ | Re-identifies on its own; events resume. |
| Events raised while connecting | ✅ | Held until Takaro accepts the server, then sent. |
| Token change without a restart | ✅ | Saving `com.takaro.valheim.cfg` reconnects within a few seconds. |
| Takaro outage | ✅ | Events are kept and delivered after reconnecting. |

### Known issues

- Players see a "Takaro" entry in the in-game player list; it is how server chat works.
- Without the mod, a kill finished by a creature after the player hit it still counts for the player.
- Ban reasons are discarded: Valheim's ban list stores only the player id.
- Given and shop items drop at the player's feet; anyone nearby could grab them first.
- Console commands only run when allowlisted; the default allowlist is just `help`.
- After upgrading, delete `BepInEx/cache/chainloader_typeloader.dat` or BepInEx may load the old plugin.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
