# Takaro SCUM Connector

A UE4SS mod that runs inside the **Windows** SCUM dedicated server. Players do not install
anything.

## Install

You need: a SCUM dedicated server on **Windows** with file access to `SCUM\Binaries\Win64` (the
folder with `SCUMServer.exe`), and a Takaro game server of type **Generic** with its registration
token.

### 1. Download

From https://takaro.io/connectors/scum, download `takaro-scum-windows-<build>-<version>.zip`, where
`<build>` is your server's Steam build. It includes the UE4SS mod loader, already set up.

### 2. Install

Stop the server. If you already use UE4SS, rename your `ue4ss` folder and `dwmapi.dll` first. Unzip
into `SCUM\Binaries\Win64`:

```text
SCUM\Binaries\Win64\dwmapi.dll
SCUM\Binaries\Win64\ue4ss\Mods\TakaroSCUM\
```

Do not change the `[Hooks]` values in `ue4ss\UE4SS-settings.ini`; the defaults crash the server
when a player joins.

### 3. Configure

Copy `ue4ss\Mods\TakaroSCUM\TakaroConfig.txt.example` to `TakaroConfig.txt`, set your token and
start the server:

```ini
REGISTRATION_TOKEN=your-registration-token-here
SERVER_NAME=My SCUM server
```

| Optional setting | What it does |
|---|---|
| `COMMAND_ALLOWLIST` | Console commands Takaro may run, without `#`, e.g. `announce,listplayers`. Empty = none. |
| `ENABLE_DESTRUCTIVE_COMMANDS` | `true` lets Takaro shut the server down. |
| `SERVER_CHAT_NAME` | Sender name on messages from Takaro, used when none is set for the server in Takaro. |

Console commands other than `announce` and `listplayers` need an admin from `AdminUsers.ini`
online. For a map background, export the client texture `SCUM/Content/ConZ_Files/UI/HUD/IslandMap`
(for example with FModel), turn it 180°, and upload it in Takaro for **The Island** with the bounds
left empty.

### 4. Check that it works

One to two minutes after start, `ue4ss\Mods\TakaroSCUM\takaro.log` shows
`identified with Takaro as game server …` and the server shows as online in Takaro. If not,
`takaro.log` and `ue4ss\UE4SS.log` say why.

**Upgrading:** stop the server and unzip the new release over `SCUM\Binaries\Win64`. Keep
`TakaroConfig.txt` and `TakaroIdentity.txt`.

## What works, what doesn't

Checked on SCUM 1.3.5.1.172191 (Steam build 26280049), 2026-10-29. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects; events from an outage are sent afterwards. |
| Player list | ✅ | SteamID64, IP and ping. |
| Player location | ✅ | |
| Teleport | ✅ | |
| Player inventory | ✅ | Rounds in a weapon are not listed. |
| Item list | ✅ | |
| Entity list | ⚠️ | Zombie and NPC names are built from codes. |
| Locations | ❌ | SCUM has no named places. |
| World map | ✅ | After the one-time map upload. |
| Send a message | ✅ | To everyone or one player. |
| Console commands | ⚠️ | Only allowed commands; most need an admin online. |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent, also offline. |
| Ban list | ✅ | |
| Give an item | ⚠️ | Drops at the player's feet when SCUM refuses the pickup. |
| Shop | ✅ | |
| Shut down the server | ✅ | Needs `ENABLE_DESTRUCTIVE_COMMANDS=true`. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | Global, local and squad. |
| Player death | ⚠️ | A player killer's name is not checked. |
| Entity killed | ✅ | |
| Server log lines | ⚠️ | Admin log lines; Takaro does not keep them. |
| Discord chat bridge | ✅ | Use `chatBridgeNoEcho`. |
| Modules | ⚠️ | `@tp` is not checked. |
| BattlEye | ✅ | Works with BattlEye on. |

## Known issues

- After a SCUM update, wait for a connector release for the new game build.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
