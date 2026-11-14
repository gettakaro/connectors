# Takaro Empyrion - Galactic Survival Connector

Connects an Empyrion - Galactic Survival dedicated server on Windows to Takaro. It is one C# mod,
loaded by the game's own mod loader in the dedicated server and its playfield servers; there is no
extra program to run. Players do not install anything.

## Install

### 1. Before you start

- An Empyrion dedicated server on **Windows** (Steam app 530870) that you can stop and start. It
  also runs under Wine on Linux.
- File access to the server's `Content\Mods` folder. On a rented host without file access, ask
  support to place the files.
- A Takaro game server of type **Generic** and its **registration token** (Takaro shows it when
  you add the server).

### 2. Download

Download the latest release from https://takaro.io/connectors/empyrion (or the newest Empyrion
release on the [GitHub releases page](https://github.com/gettakaro/connectors/releases)). You need
one file:

- `takaro-empyrion-windows-26569354-<version>.zip` for Empyrion **v1.20** (Steam build 26569354, the
  current public build). The number in the name is the server's Steam build.

Each release has a `SHA256SUMS` file to check the download with.

### 3. Install

1. Stop the server.
2. Unzip the file into the server's `Content\Mods` folder. You get:

```
<server>\Content\Mods\
    TakaroEmpyrion\
        TakaroEmpyrion.dll
        TakaroEmpyrion_Info.yaml
        TakaroConfig.txt.example
```

For example `C:\Program Files (x86)\Steam\steamapps\common\Empyrion - Dedicated Server\Content\Mods\`.
The mod loads in the dedicated server and in every playfield server (`ModTargets: Dedi, PfServer`
in `TakaroEmpyrion_Info.yaml`); leave that line as it is, or kills are not reported.

### 4. Configure

Copy `TakaroEmpyrion\TakaroConfig.txt.example` to `TakaroConfig.txt` in the same folder and fill
it in:

| Key | What to put there |
|---|---|
| `REGISTRATION_TOKEN` | Your Takaro registration token. Required. |
| `SERVER_NAME` | The name Takaro shows for this server. Default `My Empyrion Server`. |
| `IDENTITY_TOKEN` | Leave empty to derive it from `SERVER_NAME`. Once the server is in Takaro, keep it (and `SERVER_NAME`) the same, or Takaro creates a new server. |
| `TAKARO_WS_URL` | Leave `wss://connect.takaro.io/`. |
| `CHAT_SENDER_NAME` | Sender name on Takaro messages that set none. Empty = `SERVER_NAME`. |
| `CONSOLE_ALLOWLIST` | Console commands Takaro may run (first word), comma separated. Default `help,plys,list,say,kick,ban,unban,mute,servers,instances,time,gents,difficulties`. |
| `ENABLE_DESTRUCTIVE_COMMANDS` | `true` lets Takaro shut the server down. Default `false`. |
| `TELNET_PORT`, `TELNET_PASSWORD` | Leave empty: the mod reads `Tel_Port` and `Tel_Pwd` from your dedicated yaml. Console output needs the telnet console enabled there (`Tel_Enabled: true`). |
| `PROTOCOL_VERSION` | Leave `1`. `0` turns off the world map. |
| `DEBUG` | `true` writes all Takaro traffic to `Logs\takaro.log` (tokens are hidden). Default `false`. |

Start the server. Never replace `TakaroEmpyrion.dll` while the server runs: stop it first.

### 5. Check that it works

Once the server has started, `Content\Mods\TakaroEmpyrion\Logs\takaro.log` shows:

```
connected to wss://connect.takaro.io/, identify sent
identified as game server …
```

The game server now shows as online in Takaro. A wrong or missing token shows as
`Takaro rejected identify: … - check REGISTRATION_TOKEN`; without `TakaroConfig.txt` the log says
`Takaro connector disabled`.

### 6. Upgrade

Stop the server, unzip the new release over `Content\Mods` (it replaces `TakaroEmpyrion.dll` and
`TakaroEmpyrion_Info.yaml`) and start it. Keep `TakaroConfig.txt`, `players.json` and `bans.json`.
After an Empyrion update, wait for a connector release that names the new server build.

## What works, what doesn't

Checked on Empyrion v1.20 build 5229 (Steam build 26569354), 2026-11-13. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects by itself after a server restart, a network drop or a refused connection: within about 2 s after a short drop, and within the next retry (at most 60 s apart) after a longer outage. A silently dead link is noticed within about 30 s. Events from the outage are sent afterwards. |
| Player list | ✅ | Name, ping and IP. Steam players by SteamID64; Epic Games Store players by the game's own numeric id. Offline players answer with their last known data. |
| Player location | ✅ | Live position, with the planet or playfield as the map layer. |
| Teleport | ✅ | Within the player's playfield, or to another planet or playfield. |
| Player inventory | ✅ | Toolbelt and backpack, with stack sizes. |
| Item list | ✅ | All items and blocks, with their in-game names. |
| Entity list | ✅ | Creatures, with their in-game names. |
| Locations | ❌ | Empyrion has no list of named places. |
| World map | ⚠️ | One map per planet, drawn from the server's terrain. A planet gets its map once the server has generated its terrain (someone has been there). |
| Send a message | ✅ | To everyone or to one player. |
| Console commands | ⚠️ | Only commands in `CONSOLE_ALLOWLIST`, with their real output; needs the telnet console enabled. |
| Kick | ✅ | The player sees the reason. |
| Ban and unban | ⚠️ | Also offline and timed, to the nearest hour. A ban without an end time lasts ten years, the longest the game allows. |
| Ban list | ✅ | The server's bans with end times; reasons for bans made through Takaro. |
| Give an item | ✅ | Lands in the player's inventory. |
| Shop | ✅ | Buying, orders and claims work. Items land like Give an item. |
| Shut down the server | ✅ | Off by default; set `ENABLE_DESTRUCTIVE_COMMANDS=true`. The server saves first. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | Global, faction (as "team"), alliance (as "friends") and private chat. |
| Player death | ⚠️ | With position. Deaths by creatures, falls or the environment have no killer; the killer for player kills is not checked. |
| Entity killed | ✅ | Creature kills, with killer, creature and the weapon in hand. |
| Server log lines | ⚠️ | Playfield load and unload lines are sent; Takaro does not store them, but module hooks can react to them. |
| Discord chat bridge | ✅ | Both ways; use `chatBridgeNoEcho` to avoid echoes. |
| Modules | ✅ | Chat commands, teleports, scheduled jobs and hooks work. |

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
