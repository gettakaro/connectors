# Takaro Valheim Connector

A BepInEx plugin that runs inside a Valheim dedicated server and connects it to Takaro. Players
do not install anything: they join with plain, unmodified Valheim. Only reading a player's
inventory needs an optional mod on that player's game (see "Optional: inventory mod" in step 3).

## Install

### 1. Before you start

- A **Valheim dedicated server** that you can stop, start and copy files to. Releases are built
  and tested for the Linux dedicated server; Windows servers are not tested.
- **BepInExPack Valheim 5.4.2351** (`denikson/BepInExPack_Valheim` on Thunderstore) installed on
  that server. Start the server once after installing it; `BepInEx/plugins/` must exist.
- A Takaro game server of type **Generic** and its **registration token**.
- Outbound access to `wss://connect.takaro.io/`. Nothing needs to be port-forwarded.

### 2. Download

Download **`takaro-valheim-plugin.zip`** from the latest release at
https://takaro.io/connectors/valheim (or from the latest `valheim-v…` release at
<https://github.com/gettakaro/connectors/releases>; do not use the `valheim-dev` pre-release).
The release also has the same zip under its target name, for example
`takaro-valheim-plugin-linux-1.0.21-<version>.zip`, and a `SHA256SUMS` file to check the
download with.

The server builds this connector is maintained for:

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `linux-1.0.21` | 1.0.21 | linux | — | None | candidate | contract |
<!-- takaro-maint:targets:end -->

### 3. Install

Stop the server. Unzip so the `TakaroValheim` folder lands directly in `BepInEx/plugins/`, and
copy the whole folder as-is:

```
<server>/BepInEx/plugins/TakaroValheim/
    TakaroValheim.dll
    Takaro.Valheim.Core.dll
    ...the other DLLs from the zip
    manifest.json
    README.txt
```

**Optional: inventory mod for players.** Everything except player inventory works without it.
A player who wants Takaro to see their inventory installs it on their own game; nobody else
needs it, and players without it play normally on the same server.

1. On the player's PC, install **BepInExPack Valheim 5.4.2351** into the Valheim game folder
   (`steamapps/common/Valheim`) and start the game once.
2. From the same release, download **`takaro-valheim-inventory-companion.zip`** and unzip it so
   the `TakaroValheimInventoryCompanion` folder lands in `BepInEx/plugins/` of that game folder.
3. Start Valheim and join the server.

The mod holds no Takaro token and only talks to the Valheim server the player is connected to.
When it works, the server's `BepInEx/LogOutput.log` shows
`Takaro Valheim inventory companion negotiated with peer ...`. A mod from another version is
logged as ignored; the player stays connected.

### 4. Configure

The config is one file, `BepInEx/config/com.takaro.valheim.cfg`. Start the server: the connector
creates the file with a new `identityToken` and a `serverName` (the server's own name plus the
start of that identity). Paste your Takaro registration token after `registrationToken =` and
save. No restart is needed: the connector checks the file every few seconds and connects.

The file with its defaults (you can also write it yourself before the first start; the connector
then only adds an `identityToken`):

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
the other keys need a server restart. The connector only fills in an empty `identityToken` and
never changes anything else in the file. Emptying `registrationToken` disconnects the server.
`chatSenderName` is at most 128 characters.

### 5. Check that it works

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

### 6. Upgrade

Stop the server, delete `BepInEx/plugins/TakaroValheim/` and unzip the new version in its place.
Delete `BepInEx/cache/chainloader_typeloader.dat` before starting again, or BepInEx may keep
loading the old plugin. `BepInEx/config/com.takaro.valheim.cfg` is not touched, so the server
keeps its token and its identity in Takaro.

Coming from 4.2.x (settings in a `takaro.cfg` inside the plugin folder)? Do not delete the
folder: unzip the new version over it, so the connector can carry your settings over. Details
for 4.2.x and older: [UPGRADING.md](UPGRADING.md).

## What works, what doesn't

Checked on Valheim 1.0.20 (Steam build 25909431), 2026-10-22. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Dials out to Takaro; the server shows online. |
| Player list | ✅ | Name, Steam id and online state. |
| Player location | ✅ | The position the server knows; never invented. |
| Teleport | ✅ | |
| Player inventory | ⚠️ | Only for players with the optional inventory mod; not checked on this build. |
| Item list | ✅ | English in-game names (`Raspberries`, `Crude Bow`). |
| Entity list | ✅ | English in-game names (`Boar`, `Hen`). |
| Locations | ⚠️ | Sacrificial Stones, boss altars, Haldor, Hildir and the Bog Witch, after Resync Locations. Not checked on takaro.io yet. |
| World map | ⚠️ | The whole world, drawn by the server from the seed; ready about 4 minutes after the first start, at once after that. Not checked on takaro.io yet. |
| Send a message | ✅ | Broadcasts and whispers show in normal chat with the sender name. |
| Console commands | ⚠️ | Only allowlisted commands run; the default allows just `help`. |
| Kick | ✅ | The player sees "kicked"; the server stays up. |
| Ban and unban | ⚠️ | Ban drops the player and refuses rejoins, but the reason is discarded. |
| Ban list | ✅ | Matches Valheim's own ban list. |
| Give an item | ✅ | Dropped at the player's feet and picked up automatically. |
| Shop | ✅ | `@balance` works; purchases are delivered at the buyer's feet. |
| Shut down the server | ✅ | Shuts down cleanly. |
| Player joined / left | ✅ | Joined once the player has spawned; left on log out and on kick. |
| Chat messages | ✅ | Works even when a player is alone. |
| Player death | ✅ | Player and position included. |
| Entity killed | ✅ | Creature and weapon included. Counts every kill Valheim credits to the player. |
| Server log lines | ⚠️ | Sent while `enableLogEvents` is on; Takaro does not keep them as events. |
| Discord chat bridge | ⚠️ | Discord to game shows as `[D] <name>: <text>`. Game to Discord is posted but not checked in the Discord app. |
| Modules | ✅ | Chat commands, hooks, cronjobs and teleports (`@settp`, `@tp`). |
| Game name, version and icon | ✅ | The icon can take up to about two hours to appear on a fresh install. |
| Restarts and outages | ✅ | Re-identifies after a server restart; events from a Takaro outage or while connecting are sent once afterwards. |
| Single player lookup | ❌ | Not offered; Takaro uses the player list. |

## Known issues

- Players see a "Takaro" entry in the in-game player list; it is how server chat works.
- Without the inventory mod, a kill finished by a creature after the player hit it still counts
  for the player.
- Ban reasons are discarded: Valheim's ban list stores only the player id.
- Given and shop items drop at the player's feet; anyone nearby could grab them first.
- A shop order placed while Takaro has lost its connection to the server is not delivered, and
  Takaro still takes the coins.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
