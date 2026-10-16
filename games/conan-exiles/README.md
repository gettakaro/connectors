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

From the latest `conan-exiles-v*` release on https://github.com/gettakaro/connectors/releases:

| Server | Download |
|---|---|
| Linux | `takaro-conan-exiles-native-linux-25792439-<version>.zip` |
| Windows | `takaro-conan-exiles-native-windows-25792439-<version>.zip` |

Each zip holds one folder, `TakaroConanNative/`. The `Source code` links on the release page are
GitHub's own; you do not need them.

### 3. Put the files in place

Linux: unzip `TakaroConanNative/` anywhere the server user can read. Windows: copy `winmm.dll` into
`ConanSandbox\Binaries\Win64\` (next to `ConanSandboxServer-Win64-Shipping.exe`).

Keep RCON off; the connector does not use it (`Game.ini`: `[RconPlugin]` `RconEnabled=0`).

### 4. Start the server

Linux, with your usual flags:

```bash
LD_PRELOAD=/path/to/TakaroConanNative/libtakaro-conan-native.so ./ConanSandboxServer.sh -log
```

Windows: start the server as usual.

### 5. Paste the token

The connector creates its config on the first start:
`ConanSandbox/Saved/Config/Takaro/takaro.json`. Open it, set `registrationToken` to the token from
Takaro and save. Saving is enough: it connects within a few seconds, no restart needed. Optional:
`name` (the server name in Takaro).

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
- Takaro shows the server online and **Test connection** reports it reachable.
- If it is not connected, the console shows a `****` block naming the problem and the file. Fix the
  file and save it; the connector reconnects within a few seconds, no restart needed.

### 7. Map (optional)

On a Takaro with the protocol 1 game map, its map shows your players on a grid out of the box. The server files contain no map picture
and this connector ships none, so for the in-game map as background upload it once in Takaro:
open the game server's map, pick **The Exiled Lands** (or **Isle of Siptah**) and upload the image.
Do not enter any bounds; the connector already gives Takaro the exact ones.

Which image: the game's own full-screen map texture from a Conan Exiles **client** install, exported
as PNG or JPEG with an Unreal asset viewer such as FModel:

| Map | Texture (client paks) |
|---|---|
| The Exiled Lands | `ConanSandbox/Content/UI/Textures/GUIs/Map/T_FullscreenMap` (4000 × 4000) |
| Isle of Siptah | `ConanSandbox/Content/DLC/DLC_Siptah/UI/Map/T_Map_Siptah` |

Upload the whole texture: no cropping, rotating or added borders. Takaro stretches the image over the
map's bounds with the top-left pixel at the top-left (north-west) corner, so a smaller copy of the
same full image works too.

Upgrading, rollback and removal: [INSTALL.md](INSTALL.md).

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
| Locations / points of interest | ⚠️ | ⚠️ | 255 named places of the Exiled Lands, offered for Takaro's map markers; Takaro has not asked for them in a test yet. |
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
