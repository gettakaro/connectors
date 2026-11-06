# Takaro Conan Exiles Connector

A native connector that runs inside the Conan Exiles dedicated server (Linux or Windows) and
connects it to Takaro by itself: a library the Linux server preloads, or a `winmm.dll` next to the
Windows server exe. No sidecar, no RCON, no mod. Players do not install anything.

## Install

### 1. Before you start

- A Conan Exiles dedicated server (Steam app `443030`, branch `public`) on build **25792439**,
  25738716 or 25639945, that you can stop, start and copy files to. On any other build the
  connector stays connected but refuses every action and logs why.
- Linux: control of the server's start command (to set `LD_PRELOAD`).
- A Takaro game server of type **Generic** and its **registration token**.

### 2. Download

Download the latest release from https://takaro.io/connectors/conan-exiles (or from the latest
`conan-exiles-v*` release at https://github.com/gettakaro/connectors/releases; not the
`conan-exiles-dev` or `pr-*` pre-releases). Take the zip for your platform and server build:

| Server | Download |
|---|---|
| Linux | `takaro-conan-exiles-native-linux-<build>-<version>.zip` |
| Windows | `takaro-conan-exiles-native-windows-<build>-<version>.zip` |

`<build>` is 25792439, 25738716 or 25639945. Each zip holds one folder, `TakaroConanNative/`, with
the connector, a copy of the full install guide and a `SHA256SUMS` of its files.

### 3. Install

Stop the server first. Keep RCON off; the connector does not use it (`Game.ini`: `[RconPlugin]`
`RconEnabled=0`).

**Linux:** unzip `TakaroConanNative/` anywhere the server user can read, for example
`/home/steam/TakaroConanNative/`. Keep `ca-certificates.crt` next to the library; the connector
uses it for the secure connection to Takaro.

```
/home/steam/TakaroConanNative/
    libtakaro-conan-native.so
    ca-certificates.crt
```

Start the server with your usual flags and the library preloaded:

```bash
LD_PRELOAD=/path/to/TakaroConanNative/libtakaro-conan-native.so ./ConanSandboxServer.sh -log
```

In Docker, mount the folder into the server container and set `LD_PRELOAD` in the container's
environment instead. The library only activates in the server process; the launcher script ignores
it.

**Windows:** copy `winmm.dll` into `ConanSandbox\Binaries\Win64\` and start the server as usual:

```
ConanSandbox\Binaries\Win64\
    ConanSandboxServer-Win64-Shipping.exe
    winmm.dll
```

### 4. Configure

On the first start the connector creates `ConanSandbox/Saved/Config/Takaro/takaro.json`. Set
`registrationToken` to the token from Takaro and save. No restart is needed: it connects within a
few seconds. Optional: `name` (the server name in Takaro) and `serverChatName` (see below). You can also create the file before the
first start:

```json
{
  "url": "wss://connect.takaro.io/",
  "registrationToken": "<your registration token>",
  "identityToken": "",
  "name": "",
  "protocol": 1
}
```

Leave `identityToken` empty; the connector fills in a unique one. Never change it afterwards.
Leave `protocol` at `1` (it falls back to `0` on an older Takaro by itself); set `0` only if the
console says Takaro refuses protocol 1. A changed `protocol` also applies on save.

**Map (optional).** Takaro shows your players on a grid out of the box. For the in-game map as
background, upload the game's map image once in Takaro: open the game server's map, pick
**The Exiled Lands** or **Isle of Siptah** and upload; do not enter any bounds. Which image and
where to get it: [MAP.md](MAP.md).

**Server chat name (optional).** Messages from Takaro show the sender players see, `Takaro` by default. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `serverChatName` in `takaro.json` instead; the name set in Takaro wins over it.

### 5. Check that it works

- The server console shows `Takaro: connected to Takaro as "<name>"`.
- The connector's log, `ConanSandbox/Saved/Logs/TakaroConanNative.log`, shows
  `build-id … = build 25792439, … verified` (your server build) and `takaro: identified`.
- Takaro shows the server online and **Test connection** reports it reachable.
- If it is not connected, the console shows a `****` block naming the
  problem and the file. Fix the file and save it; the connector reconnects within a few seconds.

### 6. Upgrade

Stop the server, replace `libtakaro-conan-native.so` and `ca-certificates.crt` (Linux) or
`winmm.dll` (Windows) with the files from the new zip, and start the server. `takaro.json` is never
touched, so the server keeps its place in Takaro. After a game update, wait for a connector release
that names the new build; until then the connector stays connected but refuses every action.
Rollback and removal: [INSTALL.md](INSTALL.md).

## What works, what doesn't

Checked on server build 25792439 (Linux and Windows), 2026-10-09. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects after server restarts and network drops; events from an outage are queued on disk and delivered once. |
| Player list | ✅ | Steam64 id (also for Funcom-account logins), name, IP and ping. |
| Player location | ✅ | |
| Teleport | ✅ | Module teleports included. |
| Player inventory | ✅ | Backpack, hotbar and equipment with durability. |
| Item list | ✅ | In-game names. |
| Entity list | ⚠️ | In-game names; not compared against the game, which shows none. |
| Locations | ✅ | The named places of the Exiled Lands as map markers, also when nobody is online. |
| World map | ⚠️ | Exiled Lands on Linux; the background needs the one-time upload (see Configure). Windows and Isle of Siptah are not checked. |
| Send a message | ⚠️ | Broadcasts and whispers work. On Windows the server messages module is not checked. |
| Console commands | ✅ | `exit` and `quit` are refused. With nobody online an unknown command gets no answer, and the result says so. |
| Kick | ✅ | The player sees the reason. |
| Ban and unban | ✅ | Timed and permanent, also offline; banned players cannot rejoin. |
| Ban list | ✅ | With reason and expiry. |
| Give an item | ✅ | Weapons and tools arrive at full durability. |
| Shop | ⚠️ | Buying in game, orders from Takaro, bundles, offline orders and refusal without enough currency work, and so does the in-game balance. `@claim` in game is not checked. |
| Shut down the server | ✅ | Countdown in chat, then a clean exit. |
| Player joined / left | ✅ | Also for a brand-new character, kicks and shutdown. |
| Chat messages | ✅ | |
| Player death | ⚠️ | With position; a PvP killer is not checked. |
| Entity killed | ✅ | Creature name and weapon. |
| Server log lines | ✅ | |
| Discord chat bridge | ✅ | Both directions, plus module posts and join/leave notices. Use the `chatBridgeNoEcho` module. |
| Modules | ✅ | Chat commands, hooks, cronjobs and teleports. The server messages module is not checked on Windows. |

## Known issues

- Works only on server builds 25792439, 25738716 and 25639945; a game update needs a new
  connector release.
- Linux needs control of the start command for `LD_PRELOAD`; some rented hosts do not allow it.
- A death by the `Suicide` command carries no cause in its message.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
