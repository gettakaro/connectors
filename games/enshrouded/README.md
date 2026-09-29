# Takaro Enshrouded Connector

Connects an Enshrouded dedicated server to Takaro with one file: `dbghelp.dll`, placed next to
`enshrouded_server.exe`. The DLL hooks the game and talks to Takaro itself; nothing else runs next to
the server, and players do not install anything.

It is built for one exact server: game build **1024233** (Steam app 2278520, branch `public`, Steam
build 23178631), tested under **GE-Proton10-30** in `mornedhels/enshrouded-server:1.7.2-proton`. On
another game build the plugin still loads, but the features whose game code moved switch themselves
off (see "A game update can switch a feature off" below). **Keep the server on the build the release
was made for.**

## What works, what doesn't

Verified end to end on **2026-09-29** with the native connector on game build **1024233**, a real game
client and Takaro. ✅ = works, ⚠️ = works with a caveat or not re-verified, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | The game server holds an encrypted connection to Takaro and shows as reachable while it runs. |
| Player list | ✅ | `gameId` is the SteamID64. Names are Steam persona names, not in-game character names (the server never exposes those). |
| Single player lookup | ✅ | Also answers for a player who is offline but has been seen before. |
| Player location | ✅ | Matches the in-game position and follows teleports. |
| Player inventory | ✅ | Matches the in-game backpack. |
| Give an item | ✅ | The item arrives in the backpack. The game ignores the count, so the connector splits it into stacks of at most 64. |
| Item catalogue | ⚠️ | 3,609 items sync, but the names are built from the game's internal codes (the dedicated server ships no translations), so they can differ from the in-game names, e.g. "Ward (Tier 1)" for the in-game "Spectral Ward". The list also holds a few non-items such as abilities. |
| Entity catalogue | ⚠️ | 977 creature/NPC templates sync, with names built from internal codes in the same way; the list also holds some non-creatures such as containers and traps. |
| Locations / points of interest | ⚠️ | The connector serves 1,031 locations, but Takaro never asks for them. |
| Run a console command | ✅ | Enshrouded has no console; the connector provides its own set: `help`, `players`, `say`, `whisper`, `location`, `teleport`/`tp`, `inventory`, `give`, `item`, `kick`, `save-and-shutdown`. Unknown commands are refused with a hint. |
| Broadcast a message | ✅ | Everyone sees it in chat, under the character name of an online player: Enshrouded has no "server" sender. |
| Whisper a player | ⚠️ | Reaches the player; that nobody else sees it was not checked (one test account). |
| Teleport a player | ✅ | Takaro rounds to whole coordinates and the game nudges the player to the nearest free spot. |
| Kick a player | ✅ | The player must be online. Works for Admins-group players too. |
| Ban a player (timed and permanent) | ✅ | The player must be online. The game only knows permanent bans; the connector remembers the end time and lifts a timed ban itself when it expires. |
| Unban a player | ✅ | Works offline, for any player the connector has seen online once. |
| Ban list | ✅ | The game's banned accounts. Timed bans show their reason and end time; permanent bans show no reason, because the game stores none. |
| Shut the server down | ✅ | Saves first, then quits; the container's restart policy brings it back. |
| Player joined / left events | ✅ | From real joins and leaves, kicks, bans and shutdowns; a player still online when the server crashed is reported as left on the next start. |
| Player chat event | ✅ | Real player chat reaches Takaro with the player attached. |
| Player death event | ✅ | Falls and creature kills, with the position; the killer is named in the message by its internal code, e.g. `Enemy_Wildbeast_Rat_hook`. |
| Entity kill event | ⚠️ | Real kills reach Takaro, once each; the creature is its internal code and the weapon field is empty. Destroyed props are not reported. |
| Log events | ⚠️ | Sent, but Takaro does not store server log lines as events. |
| Map info | ⚠️ | Not verified in a live test. |
| Map tiles | ❌ | Takaro does not support map tiles for Generic game servers. |
| Modules: chat commands | ✅ | In-game `@` commands reach the module and answer in chat. |
| Modules: hooks | ✅ | Chat and join hooks fire and run their code. |
| Modules: cronjobs | ✅ | Scheduled and triggered runs message the server. |
| Modules: teleports (`@settp`, `@tp`) | ✅ | Saved teleports move the player. |
| Modules: server messages / onboarding | ✅ | Timed messages and the welcome message reach the game. |
| Shop: buy in game | ✅ | `@shop … buy` deducts the currency and delivers the items. |
| Shop: order in Takaro and claim | ✅ | The items reach the backpack. |
| Shop: bundle of several items | ✅ | One purchase delivers every item in the listing. |
| Shop: order while offline, claim later | ✅ | The order stays paid while the player is offline and is delivered after they rejoin. |
| Shop: not enough currency | ✅ | Refused, no order, no deduction. |
| Economy: currency and `@balance` | ✅ | Balances set in Takaro show in game. |
| Discord: game chat → Discord | ⚠️ | Player chat reaches the chat-bridge module and Takaro's Discord post succeeds; the message was not read back in Discord on this run. |
| Discord: Discord → game chat | ✅ | A real person's post in the linked channel appears in game chat; bot posts are ignored by design. |
| Discord: module hook / cronjob posts | ✅ | Hooks and cronjobs post to Discord and can edit their messages. |
| Discord: join/leave notices | ⚠️ | Takaro's Discord post succeeds on real joins and leaves; not read back in Discord on this run. |
| Discord: no echo of server messages | ✅ | Use the `chatBridgeNoEcho` module: the stock `chatBridge` re-posts Takaro's own server messages. |
| Reconnects after a restart or crash | ✅ | Back on its own within about 25 seconds after a graceful restart, a crash or a container restart. |
| Survives a network drop to Takaro | ✅ | Notices a dead link within 20 seconds and reconnects by itself, without restarting the game. |
| Events while Takaro is unreachable | ✅ | Kept on disk and delivered once the connection is back (tested with a 30-minute outage). A lost confirmation can cause one duplicate event after a reconnect. |
| Timed bans expire on their own | ✅ | Lifted within seconds of the end time. |
| A game update can switch a feature off | ✅ | A feature whose game code moved reports itself as degraded, Takaro's reachability reason names it, and everything else keeps working. |

## Install

Download the latest release: https://takaro.io/connectors/enshrouded

### 1. Before you start

- An **Enshrouded dedicated server** on game build 1024233 that you can stop, start and copy files to.
  The tested setup is the Linux Docker image `mornedhels/enshrouded-server`, where
  `enshrouded_server.exe` runs under Wine/Proton. A Windows dedicated server should work the same
  way (the plugin is a Windows DLL), but that has not been tried.
- A **Takaro** game server of type **Generic**, and its **registration token**.

### 2. Download

From the latest `enshrouded-vX.Y.Z` release at https://github.com/gettakaro/connectors/releases download
**`takaro-enshrouded-plugin-proton-1024233-<version>.zip`** and **`SHA256SUMS`**, and check the zip:

```bash
sha256sum -c SHA256SUMS --ignore-missing
```

`takaro-enshrouded-plugin.zip` on the same release is the same file under its old name. Do not
download GitHub's "Source code" archives, the `enshrouded-dev` pre-release or a `pr-<number>-enshrouded`
build.

### 3. Copy it into place

Stop the game server first: a running server holds `dbghelp.dll` open. The zip holds one folder,
`TakaroEnshrouded/`, whose contents go **next to `enshrouded_server.exe`**:

```
<server folder>/
    enshrouded_server.exe
    dbghelp.dll                  <- from the zip
    takaro/
        plugin.json              <- takaro/plugin.json.example from the zip, renamed
```

Rename `takaro/plugin.json.example` to `takaro/plugin.json` and fill in:

| Key | What to put there |
|---|---|
| `registrationToken` | The registration token of your Takaro Generic game server. Required. |
| `identityToken` | Any stable name for this server, e.g. `my-enshrouded-server`. Required; keep it the same across upgrades. |
| `name` | The server name shown in Takaro. |
| `url` | Leave `wss://connect.takaro.io/`. |
| `token` | Optional: a long random secret (e.g. `openssl rand -hex 32`) to read the plugin's diagnostics at `http://127.0.0.1:18890/health`. |

Instead of the file you can set the environment variables `TAKARO_REGISTRATION_TOKEN`,
`TAKARO_IDENTITY_TOKEN`, `TAKARO_SERVER_NAME`, `TAKARO_WS_URL` and `TAKARO_PLUGIN_TOKEN` on the game
server; a variable that is set wins over the file.

Under **Wine/Proton** the server must prefer this DLL over its own: set
`WINEDLLOVERRIDES=dbghelp=n,b` on the game server. A Windows server loads the local file already.

**With Docker**, use [`docker-compose.example.yml`](docker-compose.example.yml) and
[`.env.example`](.env.example) from this folder, and [`server/enshrouded-updater`](server/enshrouded-updater)
next to them (keep it executable). The compose file pins the image by digest, sets
`WINEDLLOVERRIDES`, passes the Takaro settings from `.env` as environment variables, and bind-mounts
the DLL read-only from `data/enshrouded/server/takaro/plugin/dbghelp.dll`:

```bash
mkdir -p data/enshrouded/server/takaro/plugin
cp TakaroEnshrouded/dbghelp.dll data/enshrouded/server/takaro/plugin/
cp .env.example .env    # fill in TAKARO_REGISTRATION_TOKEN, TAKARO_IDENTITY_ENSHROUDED and the role passwords
docker compose -f docker-compose.example.yml --env-file .env up -d
```

`server/enshrouded-updater` replaces the image's updater program, which would otherwise update the
game at every container start and move it off the build the plugin was made for. If you host the
server some other way, turn automatic game updates off there too.

### 4. Check that it worked

`<server folder>/takaro/plugin.log` shows, within about 20 seconds of the start:

```
takaro enshrouded plugin <version> starting (pid ...)
native: starting direct Takaro connection to wss://connect.takaro.io/ as '<name>' ...
native: identified with Takaro gameServerId=...
```

and the game server turns **online** in Takaro. `native: direct Takaro connection OFF: ...` names what
is missing (usually a token). `<version>` is the release you installed.

### 5. Upgrading

Stop the server, replace `dbghelp.dll`, start it again; `takaro/plugin.json` and the connector's state
in `takaro/connector-state/` stay as they are.

**From 0.5.0 (plugin + sidecar):** 0.6.0 replaces the sidecar. Stop and remove the sidecar before the
new DLL starts, because both must never connect with the same identity at once; move the tokens from
the sidecar's settings into `takaro/plugin.json` (or the game server's environment), and copy the
sidecar's `event-cursor.json` and `online-players.json` into `takaro/connector-state/` so nothing is
sent twice. [INSTALL.md](INSTALL.md) (also inside the zip) has the exact steps and the rollback to
0.5.0.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
