# Takaro Conan Exiles Connector

A native connector that runs inside the Conan Exiles dedicated server, on Linux or Windows, and
connects it to Takaro by itself. No sidecar, RCON or mod is needed. Players do not install anything.

## Install

### 1. Before you start

- A Conan Exiles dedicated server (Steam app 443030) on build 25792439 (current), 25738716 or
  25639945. On any other build the connector stays connected but refuses every action.
- Linux: control of the server's start command, to set `LD_PRELOAD`.
- In Takaro, a game server of type **Generic** and its registration token.

### 2. Download

From <https://takaro.io/connectors/conan-exiles> or the latest `conan-exiles-v*` release on
<https://github.com/gettakaro/connectors/releases>, download the zip for your platform and build:

- Linux: `takaro-conan-exiles-native-linux-<build>-<version>.zip`
- Windows: `takaro-conan-exiles-native-windows-<build>-<version>.zip`

Each zip holds one folder, `TakaroConanNative/`.

### 3. Put the files in place

Stop the server first.

- **Linux:** unzip `TakaroConanNative/` anywhere the server user can read, for example
  `/home/steam/TakaroConanNative/`. Keep `ca-certificates.crt` next to `libtakaro-conan-native.so`.
  In Docker, mount the folder into the server container.
- **Windows:** copy `winmm.dll` into `ConanSandbox\Binaries\Win64\`, next to
  `ConanSandboxServer-Win64-Shipping.exe`.

Leave RCON off (`Game.ini`, `[RconPlugin]`, `RconEnabled=0`); the connector does not use it.

### 4. Start the server

Linux, with your usual flags (in Docker, set `LD_PRELOAD` in the container environment instead):

```bash
LD_PRELOAD=/path/to/TakaroConanNative/libtakaro-conan-native.so ./ConanSandboxServer.sh -log
```

Windows: start the server as usual.

### 5. Set the token

On first start the connector creates `ConanSandbox/Saved/Config/Takaro/takaro.json`. Set
`registrationToken` to your token and save; it connects within a few seconds, no restart needed.
Optionally set `name`, the server name in Takaro. You can also create the file before the first start:

```json
{
  "url": "wss://connect.takaro.io/",
  "registrationToken": "<your registration token>",
  "identityToken": "",
  "name": "",
  "protocol": 1
}
```

Leave `identityToken` empty; the connector fills it in. Do not change it afterwards. Set `protocol`
to `0` only if the console says Takaro refuses protocol 1.

### 6. Check that it worked

The server console shows `Takaro: connected to Takaro as "<name>"`, and Takaro shows the server
online. If it is not connected, the console shows a `****` block naming the problem. The connector's
log is `ConanSandbox/Saved/Logs/TakaroConanNative.log`.

### Map (optional)

Takaro shows players on a grid by default. For the in-game map as background, open the game
server's map in Takaro, pick **The Exiled Lands** or **Isle of Siptah** and upload the game's map
image without entering bounds. Which image to use: [MAP.md](MAP.md).

### Update

Stop the server, replace `libtakaro-conan-native.so` and `ca-certificates.crt` (Linux) or
`winmm.dll` (Windows) with the new files, and start it. `takaro.json` is kept. After a game update,
wait for a connector release that names the new build. Rollback and removal: [INSTALL.md](INSTALL.md).

## What works, what doesn't

Tested on build 25792439 on 2026-10-26 (Linux); Windows last tested on 2026-10-09.

| Feature | Status | Note |
|---|---|---|
| Connection and reconnect | ✅ | Survives server restarts and network drops. |
| Player list and lookup | ✅ | Steam ID, name, IP and ping. |
| Player location and inventory | ✅ | Backpack, hotbar and equipment. |
| Give item | ✅ | Weapons and tools arrive at full durability. |
| Item and entity catalogue | ✅ | In-game names. |
| Locations | ✅ | Named places of the Exiled Lands, also with nobody online. |
| Console commands | ✅ | Unknown commands fail; `exit` and `quit` are refused. |
| Broadcast and whisper | ✅ | |
| Teleport | ✅ | |
| Kick | ✅ | The player sees the reason. |
| Ban, unban, ban list | ✅ | Timed and permanent, also offline; banned players cannot rejoin. |
| Shutdown | ✅ | Countdown in chat, then a clean exit. |
| Join, leave and chat events | ✅ | |
| Death events | ✅ | Cause and position. |
| Entity kill events | ✅ | Creature name and weapon. |
| Log events | ✅ | |
| Map | ⚠️ | Needs Takaro protocol 1. Siptah and Windows untested. |
| Game name, version and icon | ⚠️ | Needs Takaro protocol 1. |
| Modules (commands, hooks, cronjobs, teleports) | ✅ | |
| Shop and economy | ✅ | Bundles and offline orders included. |
| Shop `@claim` in game | ⚠️ | Not tested. |
| Discord chat both ways | ✅ | Use the `chatBridgeNoEcho` module. |
| Events during a Takaro outage | ✅ | Queued on disk and delivered after reconnect. |
| PvP kills, whisper privacy | ⚠️ | Not tested with two players. |

## Known issues

- A game update needs a new connector release; until then actions are refused.
- Some rented hosts do not let you set `LD_PRELOAD` on Linux.
- A death by the `Suicide` command carries no cause.
