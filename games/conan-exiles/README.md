# Takaro Conan Exiles Connector

A native connector that runs inside the Conan Exiles dedicated server and connects it to Takaro by
itself: a library the Linux server preloads, or a `winmm.dll` next to the Windows server exe. No
sidecar, no RCON, no mod. Players do not install anything.

It is built for exact server builds: **Conan Exiles Dedicated Server build 25792439** (Steam app
`443030`, branch `public`, the current release) and the previous builds 25738716 and 25639945,
Linux and Windows. On any other build it stays connected but refuses every action and logs why.

## Install

### 1. Before you start

- A Conan Exiles dedicated server on build 25792439 (or 25738716, 25639945) that you can stop, start and
  copy files to.
- Linux: control of the server's start command (to set `LD_PRELOAD`).
- A Takaro game server of type **Generic** and its **registration token**.

### 2. Download

Download the latest release: <https://takaro.io/connectors/conan-exiles>. The same files are on the
latest `conan-exiles-v*` release at <https://github.com/gettakaro/connectors/releases>. Do not use
the `conan-exiles-dev` or `pr-*` pre-releases. Take the zip for your platform and your server build:

| Server | Download |
|---|---|
| Linux | `takaro-conan-exiles-native-linux-<build>-<version>.zip` |
| Windows | `takaro-conan-exiles-native-windows-<build>-<version>.zip` |

`<build>` is 25792439, 25738716 or 25639945. Each zip holds one folder, `TakaroConanNative/`, with
the connector (`libtakaro-conan-native.so` and `ca-certificates.crt` on Linux, `winmm.dll` on
Windows), a copy of the full install guide and a `SHA256SUMS` of its files. The `Source code` links
on the GitHub release page are GitHub's own; you do not need them.

### 3. Put the files in place

Stop the server first.

Linux: unzip `TakaroConanNative/` anywhere the server user can read, for example
`/home/steam/TakaroConanNative/`. Keep `ca-certificates.crt` next to `libtakaro-conan-native.so`:
the connector uses it for the secure connection to Takaro. In Docker, mount the folder into the
server container.

Windows: copy `TakaroConanNative\winmm.dll` into `ConanSandbox\Binaries\Win64\` (next to
`ConanSandboxServer-Win64-Shipping.exe`).

Keep RCON off; the connector does not use it (`Game.ini`: `[RconPlugin]` `RconEnabled=0`).

### 4. Start the server

Linux, with your usual flags:

```bash
LD_PRELOAD=/path/to/TakaroConanNative/libtakaro-conan-native.so ./ConanSandboxServer.sh -log
```

In Docker, set `LD_PRELOAD` in the server container's environment instead. The library only
activates in the server process; the launcher script ignores it.

Windows: start the server as usual.

### 5. Paste the token

The connector creates its config on the first start:
`ConanSandbox/Saved/Config/Takaro/takaro.json`. Open it, set `registrationToken` to the token from
Takaro and save. Saving is enough: it connects within a few seconds, no restart needed. Optional:
`name` (the server name in Takaro) and `serverChatName` (the sender players see on messages from
Takaro instead of `Takaro`; the chat name set on the game server in Takaro wins over it).

You can also create the file yourself before the first start, with this content:

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
`protocol`: `1` (default) offers Takaro protocol 1 (game map, event acks) and falls back to 0 on an
older Takaro; set `0` only if the console says Takaro refuses protocol 1. Saving a changed `protocol`
reconnects, no restart needed.

### 6. Check that it worked

- The server console shows `Takaro: connected to Takaro as "<name>"`.
- The connector's own log, `ConanSandbox/Saved/Logs/TakaroConanNative.log`, shows
  `build-id … = build 25792439, … verified` (your server build) and `takaro: identified`.
- Takaro shows the server online and **Test connection** reports it reachable.
- If it is not connected, the console shows a `****` block naming the problem and the file. Fix the
  file and save it; the connector reconnects within a few seconds, no restart needed.

### 7. Map (optional)

On a Takaro with the protocol 1 game map, the map shows your players on a grid out of the box. For
the in-game map as background, upload the game's map image once in Takaro (open the game server's
map, pick **The Exiled Lands** or **Isle of Siptah**, upload; do not enter any bounds). Which image
and where to get it: [MAP.md](MAP.md).

### 8. Upgrade to a later release

Stop the server, replace `libtakaro-conan-native.so` and `ca-certificates.crt` (Linux) or
`winmm.dll` (Windows) with the files from the new zip, and start the server. `takaro.json` is never
touched, so the server keeps its place in Takaro. When the game updates to a new server build, wait
for a connector release that names the new build; until then the connector stays connected but
refuses every action.

Rollback and removal: [INSTALL.md](INSTALL.md).

## What works, what doesn't

✅ = works, ⚠️ = works with a caveat or not yet verified, ❌ = unsupported, N/A = does not apply.
Verified with a real client on build 25639945 (Linux 2026-10-03/04, Windows 2026-10-04/05) and
re-verified on build 25738716 (Linux 2026-10-06, Windows 2026-10-07) and on build 25792439 (Linux and
Windows 2026-10-09).

| What | Linux | Windows | Notes |
|---|---|---|---|
| Connection & heartbeat | ✅ | ✅ | Connects to Takaro itself; no sidecar, no RCON. |
| Player list | ✅ | ✅ | Steam64 id (also for Funcom-account logins), name, IP, ping. |
| Single player lookup | ✅ | ✅ | Offline players answer their last known record. |
| Player location | ✅ | ✅ | |
| Player inventory | ✅ | ✅ | Backpack, hotbar and equipment with durability. |
| Give an item | ✅ | ✅ | Weapons and tools arrive at full durability. |
| Item catalogue | ✅ | ✅ | 7521 items with in-game names. |
| Entity catalogue | ⚠️ | ⚠️ | In-game names; the client shows none to compare with. |
| Locations / points of interest | ✅ | ✅ | 255 named places of the Exiled Lands as Takaro map markers, also when nobody is online. |
| Run a console command | ✅ | ✅ | Unknown commands fail while a player is online (with nobody online the engine console gives no answer, and the result says so); `exit`/`quit` are refused. |
| Broadcast a message | ✅ | ✅ | |
| Whisper a player | ✅ | ✅ | |
| Teleport a player | ✅ | ✅ | Module teleports included. |
| Kick a player | ✅ | ✅ | The player sees the reason. |
| Ban a player (timed and permanent) | ✅ | ✅ | Also offline; banned players cannot rejoin. |
| Unban a player | ✅ | ✅ | |
| Ban list | ✅ | ✅ | Includes the reason and expiry. |
| Shut the server down | ✅ | ✅ | Countdown in chat, then a clean exit. |
| Player joined event | ✅ | ✅ | Also for a brand-new character. |
| Player left event | ✅ | ✅ | Also for kicks and shutdown. |
| Player chat event | ✅ | ✅ | |
| Player death event | ⚠️ | ⚠️ | With position; a PvP killer is not yet verified. |
| Entity kill event | ✅ | ✅ | Creature name and weapon. |
| Log events | ✅ | ✅ | |
| World map with player positions | ✅ | ⚠️ | Exiled Lands; the background needs the one-time upload in step 7. Windows and Isle of Siptah not yet verified. |
| Modules: chat commands | ✅ | ✅ | |
| Modules: hooks | ✅ | ✅ | |
| Modules: cronjobs | ✅ | ✅ | |
| Modules: teleports (`@settp`, `@tp`, …) | ✅ | ✅ | |
| Modules: server messages | ✅ | ⚠️ | Windows: the module itself not run; module cron messages to game work. |
| Shop: buy in game | ✅ | ✅ | |
| Shop: order in Takaro and claim | ✅ | ✅ | |
| Shop: `@claim` in game | ⚠️ | ⚠️ | Not verified (needs Takaro account linking). |
| Shop: bundle of several items | ✅ | ✅ | |
| Shop: order while offline, claim later | ✅ | ✅ | |
| Shop: not enough currency | ✅ | ✅ | Refused, nothing deducted. |
| Economy: balance in game | ✅ | ✅ | |
| Discord: game chat → Discord | ✅ | ✅ | Use the `chatBridgeNoEcho` module. |
| Discord: Discord → game chat | ✅ | ✅ | |
| Discord: module hook / cronjob posts | ✅ | ✅ | |
| Discord: join/leave notices | ✅ | ✅ | |
| Discord: no echo of server messages | ✅ | ✅ | Use the `chatBridgeNoEcho` module. |
| Events while the Takaro connection is down | ✅ | ✅ | Queued on disk, delivered once after reconnect. |
| Reconnects after a server restart | ✅ | ✅ | |
| Survives a network drop to Takaro | ✅ | ✅ | |
| Unknown server build | ✅ | ✅ | Stays connected, refuses actions, says why. |


### Known issues

- Works only on server builds 25792439, 25738716 and 25639945; a game update needs a new connector release.
- Linux needs control of the start command for `LD_PRELOAD`; some rented hosts do not allow it.
- A death by the `Suicide` command carries no cause in its message.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
