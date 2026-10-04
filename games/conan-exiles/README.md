# Takaro Conan Exiles Connector

A native connector that runs inside the Conan Exiles dedicated server and connects it to Takaro by
itself: a library the Linux server preloads, or a `winmm.dll` next to the Windows server exe. No
sidecar, no RCON, no mod. Players do not install anything.

It is built for one exact server build: **Conan Exiles Dedicated Server build 25639945** (Steam app
`443030`, branch `public`), Linux and Windows. On any other build it stays connected but refuses
every action and logs why.

## Install

### 1. Before you start

- A Conan Exiles dedicated server on build 25639945 that you can stop, start and copy files to.
- Linux: control of the server's start command (to set `LD_PRELOAD`).
- A Takaro game server of type **Generic** and its **registration token**.

### 2. Download

From the latest `conan-exiles-v*` release on https://github.com/gettakaro/connectors/releases:

| Server | Download |
|---|---|
| Linux | `takaro-conan-exiles-native-linux-25639945-<version>.zip` |
| Windows | `takaro-conan-exiles-native-windows-25639945-<version>.zip` |

Each zip holds one folder, `TakaroConanNative/`. The `Source code` links on the release page are
GitHub's own; you do not need them.

### 3. Put the files in place

Linux: unzip `TakaroConanNative/` anywhere the server user can read, then copy two files:

```bash
mkdir -p <server>/ConanSandbox/Saved/Config/Takaro
cp TakaroConanNative/takaro.json.example <server>/ConanSandbox/Saved/Config/Takaro/takaro.json
cp TakaroConanNative/ca-certificates.crt <server>/ConanSandbox/Saved/Config/Takaro/
```

Windows: copy `winmm.dll` into `ConanSandbox\Binaries\Win64\` (next to
`ConanSandboxServer-Win64-Shipping.exe`), and `takaro.json.example` to
`ConanSandbox\Saved\Config\Takaro\takaro.json`.

### 4. Configure

Edit `takaro.json`:

| Key | Value |
|---|---|
| `url` | `wss://connect.takaro.io/` |
| `identityToken` | A name you choose for this server, unique in your domain. Never change it. |
| `registrationToken` | The registration token from Takaro. |
| `name` | The server name Takaro shows. |
| `caFile` | Linux only: `Config/Takaro/ca-certificates.crt` |

Keep RCON off; the connector does not use it (`Game.ini`: `[RconPlugin]` `RconEnabled=0`).

### 5. Start the server

Linux, with your usual flags:

```bash
LD_PRELOAD=/path/to/TakaroConanNative/libtakaro-conan-native.so ./ConanSandboxServer.sh -log
```

Windows: start the server as usual.

### 6. Check that it worked

- `ConanSandbox/Saved/Logs/TakaroConanNative.log` shows `takaro: identified`.
- Takaro shows the server online and **Test connection** reports it reachable.

Upgrading, moving over from the old Node.js bridge, rollback and removal:
[INSTALL.md](INSTALL.md). The bridge zip is still published for one more release cycle but is
deprecated and gets no fixes.

## What works, what doesn't

✅ = works, ⚠️ = works with a caveat or not yet verified, ❌ = unsupported, N/A = does not apply.
Verified on build 25639945 with a real client: Linux 2026-10-03/04, Windows 2026-10-03.

| What | Linux | Windows | Notes |
|---|---|---|---|
| Connection & heartbeat | ✅ | ✅ | Connects to Takaro itself; no sidecar, no RCON. |
| Player list | ✅ | ⚠️ | Steam64 id, name, IP, ping. Windows: not yet verified. |
| Single player lookup | ✅ | ⚠️ | Offline players answer their last known record. |
| Player location | ✅ | ⚠️ | |
| Player inventory | ✅ | ⚠️ | Backpack, hotbar and equipment with durability. |
| Give an item | ✅ | ⚠️ | Weapons and tools arrive at full durability. |
| Item catalogue | ✅ | ⚠️ | 7521 items with in-game names. |
| Entity catalogue | ⚠️ | ⚠️ | In-game names; the client shows none to compare with. |
| Locations / points of interest | N/A | N/A | Takaro never asks for this list. |
| Run a console command | ✅ | ⚠️ | Unknown commands fail; `exit`/`quit` are refused. |
| Broadcast a message | ✅ | ✅ | |
| Whisper a player | ✅ | ✅ | |
| Teleport a player | ✅ | ⚠️ | Module teleports included. |
| Kick a player | ✅ | ⚠️ | The player sees the reason. |
| Ban a player (timed and permanent) | ✅ | ⚠️ | Also offline; banned players cannot rejoin. |
| Unban a player | ✅ | ⚠️ | |
| Ban list | ✅ | ⚠️ | Includes the reason and expiry. |
| Shut the server down | ✅ | ⚠️ | Countdown in chat, then a clean exit. |
| Player joined event | ✅ | ⚠️ | Also for a brand-new character. |
| Player left event | ✅ | ⚠️ | Also for kicks and shutdown. |
| Player chat event | ✅ | ⚠️ | |
| Player death event | ⚠️ | ⚠️ | With position; a PvP killer is not yet verified. |
| Entity kill event | ✅ | ⚠️ | Creature name and weapon. |
| Log events | ✅ | ⚠️ | |
| Map info | N/A | N/A | Takaro has no map info for Generic game servers. |
| Map tiles | N/A | N/A | Takaro has no map tiles for Generic game servers. |
| Modules: chat commands | ✅ | ⚠️ | |
| Modules: hooks | ✅ | ⚠️ | |
| Modules: cronjobs | ✅ | ⚠️ | |
| Modules: teleports (`@settp`, `@tp`, …) | ✅ | ⚠️ | |
| Modules: server messages | ✅ | ⚠️ | |
| Shop: buy in game | ✅ | ⚠️ | |
| Shop: order in Takaro and claim | ✅ | ⚠️ | |
| Shop: `@claim` in game | ⚠️ | ⚠️ | Not verified (needs Takaro account linking). |
| Shop: bundle of several items | ✅ | ⚠️ | |
| Shop: order while offline, claim later | ✅ | ⚠️ | |
| Shop: not enough currency | ✅ | ⚠️ | Refused, nothing deducted. |
| Economy: balance in game | ✅ | ⚠️ | |
| Discord: game chat → Discord | ✅ | ⚠️ | Use the `chatBridgeNoEcho` module. |
| Discord: Discord → game chat | ✅ | ⚠️ | |
| Discord: module hook / cronjob posts | ✅ | ⚠️ | |
| Discord: join/leave notices | ✅ | ⚠️ | |
| Discord: no echo of server messages | ✅ | ⚠️ | Use the `chatBridgeNoEcho` module. |
| Events while the Takaro connection is down | ✅ | ⚠️ | Queued on disk, delivered once after reconnect. |
| Reconnects after a server restart | ✅ | ⚠️ | |
| Survives a network drop to Takaro | ✅ | ⚠️ | |
| Unknown server build | ✅ | ⚠️ | Stays connected, refuses actions, says why. |

On Windows, ⚠️ means not yet verified on Windows.

### Known issues

- Works only on server build 25639945; a game update needs a new connector release.
- Linux needs control of the start command for `LD_PRELOAD`; some rented hosts do not allow it.
- The Windows build is not fully tested yet: only the connection and messages are proven.
- A death by the `Suicide` command carries no cause in its message.
- After moving from the bridge, its old catalogue rows (class names) stay in Takaro; Takaro never deletes them.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
