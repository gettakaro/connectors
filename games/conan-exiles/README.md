# Takaro Conan Exiles Connector

A native connector that runs inside the Conan Exiles dedicated server: a preloaded library on
Linux, a `winmm.dll` on Windows. No RCON and no mod. Players do not install anything.

## Install

You need: a Conan Exiles dedicated server on build **25792439**, 25738716 or 25639945 (on Linux,
control of its start command), and a Takaro game server of type **Generic** with its registration
token.

### 1. Download

From https://takaro.io/connectors/conan-exiles, download the zip for your platform and build:

| Server | Download |
|---|---|
| Linux | `takaro-conan-exiles-native-linux-<build>-<version>.zip` |
| Windows | `takaro-conan-exiles-native-windows-<build>-<version>.zip` |

Do not use the `conan-exiles-dev` or `pr-*` pre-releases.

### 2. Install

Stop the server. Keep RCON off (`Game.ini`: `[RconPlugin]` `RconEnabled=0`).

**Linux:** unzip `TakaroConanNative/` anywhere the server user can read, keeping
`ca-certificates.crt` next to the library, and start the server with the library preloaded:

```bash
LD_PRELOAD=/path/to/TakaroConanNative/libtakaro-conan-native.so ./ConanSandboxServer.sh -log
```

In Docker, mount the folder and set `LD_PRELOAD` in the container's environment.

**Windows:** copy `winmm.dll` next to the server exe and start the server as usual:

```text
ConanSandbox\Binaries\Win64\winmm.dll
```

### 3. Configure

On first start the connector creates `ConanSandbox/Saved/Config/Takaro/takaro.json`. Set
`registrationToken` and save; it connects within a few seconds, no restart needed.

```json
{
  "url": "wss://connect.takaro.io/",
  "registrationToken": "<your registration token>",
  "identityToken": "",
  "name": "",
  "protocol": 1
}
```

Leave `identityToken` empty; the connector fills it in. Never change it afterwards.

| Optional setting | What it does |
|---|---|
| `name` | Server name in Takaro. |
| `serverChatName` | Sender name on messages from Takaro, used when none is set for the server in Takaro. |
| Map image | Upload the game's map in Takaro for a map background; see [MAP.md](MAP.md). |

### 4. Check that it works

The server console shows `Takaro: connected to Takaro as "<name>"` and the server shows as online
in Takaro. If it is not connected, the console shows a `****` block naming the problem and the file.

**Upgrading:** stop the server and replace `libtakaro-conan-native.so` and `ca-certificates.crt`
(Linux) or `winmm.dll` (Windows). `takaro.json` stays.

## What works, what doesn't

Checked on server build 25792439 (Linux and Windows), 2026-10-09. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Reconnects; events from an outage are delivered afterwards. |
| Player list | ✅ | Steam64 id, IP and ping. |
| Player location | ✅ | |
| Teleport | ✅ | |
| Player inventory | ✅ | |
| Item list | ✅ | |
| Entity list | ⚠️ | Not compared against the game. |
| Locations | ✅ | Named places of the Exiled Lands. |
| World map | ⚠️ | Exiled Lands on Linux; Windows and Isle of Siptah not checked. |
| Send a message | ✅ | Broadcasts and whispers. |
| Console commands | ✅ | `exit` and `quit` are refused. |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent, also offline. |
| Ban list | ✅ | |
| Give an item | ✅ | |
| Shop | ⚠️ | `@claim` in game is not checked. |
| Shut down the server | ✅ | Countdown in chat, then a clean exit. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | |
| Player death | ⚠️ | A PvP killer is not checked. |
| Entity killed | ✅ | |
| Server log lines | ✅ | |
| Discord chat bridge | ✅ | Use the `chatBridgeNoEcho` module. |
| Modules | ✅ | |

## Known issues

- Only server builds 25792439, 25738716 and 25639945 work; on other builds the connector stays
  connected but refuses every action until a new release.
- Linux needs control of the start command for `LD_PRELOAD`; some rented hosts do not allow it.

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
