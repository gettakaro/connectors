# Takaro Enshrouded Connector

Connects an Enshrouded dedicated server to Takaro with one file: `dbghelp.dll`, placed next to
`enshrouded_server.exe`. The DLL hooks the game and talks to Takaro itself, so it is the only thing the
server needs; players join as usual.

It supports **Enshrouded 1.0** (game build **1267041**, hotfix 1.0.0.1, Steam build 25967645): tested
natively on a Windows dedicated server and under **GE-Proton11-9** in
`mornedhels/enshrouded-server:dev-proton` (the older GE-Proton10-30 image cannot start the 1.0 server:
`No SteamUtils011`). Everything that changes between game builds lives in one data file,
`takaro/enshrouded-gamedata.json`. **The item and creature catalogue and `giveItem` are compiled for 1.0
and are switched off (degraded) on any other game build, including the earlier build 1024233, which is
no longer a supported target.** A build the data file does not know keeps the hooks that still match;
the rest switch themselves off (see "A game update can switch a feature off" below). **Keep the server on
a build the connector supports.**

## What works, what doesn't

Verified end to end on **2026-09-29** on game build **1024233** (Proton, plugin 0.7.x), and re-verified on
**2026-10-15** on game build **1267041** (Enshrouded 1.0, Windows), each with a real game client and
Takaro. Rows marked *(1.0 ✓)* were re-checked on 1.0; the others were verified on 1024233 only.
✅ = works, ⚠️ = works with a caveat or not re-verified, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat *(1.0 ✓)* | ✅ | The game server holds an encrypted connection to Takaro and shows as reachable while it runs. |
| Player list *(1.0 ✓)* | ✅ | `gameId` is the SteamID64. Names are Steam persona names, not in-game character names (the server never exposes those). |
| Single player lookup | ✅ | Also answers for a player who is offline but has been seen before. |
| Player location *(1.0 ✓)* | ✅ | Matches the in-game position and follows teleports. |
| Player inventory *(1.0 ✓)* | ✅ | Matches the in-game backpack. |
| Give an item *(1.0 ✓)* | ✅ | The item arrives in the backpack. The game ignores the count, so the connector splits it into stacks of at most 64. |
| Item catalogue *(1.0 ✓)* | ✅ | On 1.0, 4,005 of 4,262 items sync with their in-game English names, e.g. "Spectral Ward" (1024233: 3,317). Items the game never names and abilities are left out; giving an item by its code works for every item. |
| Entity catalogue | ⚠️ | On 1.0, 477 creature and NPC templates sync (1024233: 397). Bosses, NPCs and fish have their in-game names (e.g. "Armored Fell Thunderbrute", "Oswald Anders"). The game shows no name for other creatures, so they get a readable name built from the internal code, e.g. "Rat". |
| Locations / points of interest | ⚠️ | The connector serves 1,031 locations. Takaro may show them as markers on the map; this was not checked. |
| Run a console command *(1.0 ✓)* | ✅ | Enshrouded has no console; the connector provides its own set: `help`, `players`, `say`, `whisper`, `location`, `teleport`/`tp`, `inventory`, `give`, `item`, `kick`, `save-and-shutdown`. Unknown commands are refused with a hint. |
| Broadcast a message *(1.0 ✓)* | ✅ | Everyone sees it in chat, under the character name of an online player: Enshrouded has no "server" sender. |
| Whisper a player | ⚠️ | Reaches the player; that nobody else sees it was not checked (one test account). |
| Teleport a player *(1.0 ✓)* | ✅ | Takaro rounds to whole coordinates. On 1024233 the game nudges the player to the nearest free spot; on 1.0 that search snaps the player back, so the player lands exactly on the coordinates: pick a spot above ground. |
| Kick a player *(1.0 ✓)* | ✅ | The player must be online. Works for Admins-group players too. |
| Ban a player (timed and permanent) *(1.0 ✓ permanent)* | ✅ | The player must be online. The game only knows permanent bans; the connector remembers the end time and lifts a timed ban itself when it expires. |
| Unban a player *(1.0 ✓)* | ✅ | Works offline, for any player the connector has seen online once. |
| Ban list *(1.0 ✓)* | ✅ | The game's banned accounts. Timed bans show their reason and end time; permanent bans show no reason, because the game stores none. |
| Shut the server down | ✅ | Saves first, then quits; the container's restart policy brings it back. |
| Player joined / left events *(1.0 ✓)* | ✅ | From real joins and leaves, kicks, bans and shutdowns; a player still online when the server crashed is reported as left on the next start. |
| Player chat event *(1.0 ✓)* | ✅ | Real player chat reaches Takaro with the player attached. |
| Player death event *(1.0 ✓)* | ✅ | Falls and creature kills, with the position; the creature that killed the player is named in the message the same way as in the entity catalogue, e.g. "Limon was killed by Rat". |
| Entity kill event *(1.0 ✓)* | ⚠️ | Real kills reach Takaro, once each, with the weapon's in-game name (melee and bow tested). The creature is named the same way as in the entity catalogue, e.g. "Rat". Kills with magic are not tested yet. Destroyed props are not reported. |
| Log events | ⚠️ | Sent, but Takaro does not store server log lines as events. |
| Map | ⚠️ | Needs Takaro protocol 1 (proven on Takaro staging, not yet on takaro.io). The connector tells Takaro the size and orientation of the world ("Embervale", 10,240 m square, north up). The server has no map picture of its own, so you upload one image once on the game server's map page in Takaro (see INSTALL.md, "Map"); player dots then sit at the right spot on it. Without an upload Takaro shows a grid with the player dots. |
| Map on an older Takaro | ⚠️ | An older Takaro is today's takaro.io. A Takaro without protocol 1 shows a map only from a tile pack in `takaro/map/` that you render yourself (see INSTALL.md). |
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
| Discord: game chat → Discord *(1.0 ✓)* | ⚠️ | Player chat reaches the chat-bridge module and Takaro's Discord post succeeds (HTTP 200 on 1.0); the message was not read back in Discord. |
| Discord: Discord → game chat *(1.0 ✓)* | ✅ | A real person's post in the linked channel appears in game chat; bot posts are ignored by design. |
| Discord: module hook / cronjob posts | ✅ | Hooks and cronjobs post to Discord and can edit their messages. |
| Discord: join/leave notices | ⚠️ | Takaro's Discord post succeeds on real joins and leaves; not read back in Discord on this run. |
| Discord: no echo of server messages | ✅ | Use the `chatBridgeNoEcho` module: the stock `chatBridge` re-posts Takaro's own server messages. |
| Reconnects after a restart or crash *(1.0 ✓)* | ✅ | Back on its own within about 25 seconds after a graceful restart, a crash or a container restart. |
| Survives a network drop to Takaro *(1.0 ✓)* | ✅ | Notices a dead link within 20 seconds and reconnects by itself, without restarting the game. |
| Events while Takaro is unreachable | ✅ | Kept on disk and delivered once the connection is back (tested with a 30-minute outage). A lost confirmation can cause one duplicate event after a reconnect. |
| Timed bans expire on their own | ✅ | Lifted within seconds of the end time. |
| A game update can switch a feature off | ✅ | A feature whose game code moved reports itself as degraded, Takaro's reachability reason names it, and everything else keeps working. |

## Install

Download the latest release: https://takaro.io/connectors/enshrouded

### 1. Before you start

- An **Enshrouded dedicated server** you can stop, start and copy files to, on a game build the
  plugin's game data knows: **1267041 (Enshrouded 1.0 hotfix 1.0.0.1)**. Tested natively on
  Windows and under Proton in the Linux Docker image `mornedhels/enshrouded-server:dev-proton`.
  On another build the plugin logs `gamedata: no section for build <n>` and uses its older heuristics;
  features whose code moved report themselves degraded. Adding a section to
  `takaro/enshrouded-gamedata.json` (see DEVELOPMENT.md) brings them back without a new DLL.
- A **Takaro** game server of type **Generic**, and its **registration token**.

### 2. Download

From the latest `enshrouded-vX.Y.Z` release at https://github.com/gettakaro/connectors/releases download
**`takaro-enshrouded-plugin-proton-1267041-<version>.zip`** and **`SHA256SUMS`**, and check the zip. The
name carries the build the release target was first pinned to; the same `dbghelp.dll` runs on every
build in its game data file, including Enshrouded 1.0 on Windows:

```bash
sha256sum -c SHA256SUMS --ignore-missing
```

`takaro-enshrouded-plugin.zip` on the same release is the same file under its old name. Use these
release assets; the `enshrouded-dev` and `pr-<number>-enshrouded` pre-releases are test builds.

### 3. Copy it into place

Stop the game server first: a running server holds `dbghelp.dll` open. The zip holds one folder,
`TakaroEnshrouded/`; copy its **`dbghelp.dll` next to `enshrouded_server.exe`** and start the server.

The connector keeps its whole configuration in one file, **`takaro/plugin.json` next to
`enshrouded_server.exe`**. It creates the file at the first start, with a unique `identityToken`, and
the console says so. Open it, paste the registration token of your Takaro Generic game server into
`registrationToken` and save: the connector connects within a few seconds while the server keeps running. To have
it connect at the first start instead, create the file yourself before starting:

```json
{
  "registrationToken": "<your registration token>",
  "identityToken": "",
  "name": "",
  "url": "wss://connect.takaro.io/",
  "protocol": 1,
  "token": "",
  "mapDir": "",
  "caFile": ""
}
```

```
<server folder>/
    enshrouded_server.exe
    dbghelp.dll                  <- from the zip
    takaro/
        plugin.json              <- created by the connector; your settings
```

| Key | What to put there |
|---|---|
| `registrationToken` | The registration token of your Takaro Generic game server. Required. |
| `identityToken` | Leave it empty: the connector writes a unique identity there. Keep it once set; it is what Takaro knows this server by. |
| `name` | The server name shown in Takaro. Left empty, it is your server's name plus the start of the identity, because Takaro needs a different name for every game server. |
| `url` | Leave `wss://connect.takaro.io/`. |
| `protocol` | Leave `1`: the connector offers Takaro protocol 1 (the map) and falls back to 0 on a Takaro without it. `0` speaks protocol 0 only. |
| `token` | Optional: a long random secret (e.g. `openssl rand -hex 32`) to read the plugin's diagnostics at `http://127.0.0.1:18890/health`. Needs a restart. |
| `mapDir` | Optional: the folder of a protocol 0 map tile pack. Empty: `takaro/map`. Needs a restart. |
| `caFile` | Optional: a CA file, only for a Takaro behind your own certificate authority. Empty: the system's trust. Needs a restart. |

A change to `registrationToken`, `identityToken`, `name`, `url` or `protocol` applies within a few
seconds: it reconnects, an emptied token disconnects. `token`, `mapDir` and `caFile` need a restart.

Instead of the file you can set the environment variables `TAKARO_REGISTRATION_TOKEN`,
`TAKARO_IDENTITY_TOKEN`, `TAKARO_SERVER_NAME`, `TAKARO_WS_URL` and `TAKARO_PLUGIN_TOKEN` on the game
server; a variable that is set wins over the file (a changed variable needs a restart).

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
cp .env.example .env    # fill in TAKARO_REGISTRATION_TOKEN and the role passwords
docker compose -f docker-compose.example.yml --env-file .env up -d
```

`server/enshrouded-updater` replaces the image's updater program, which would otherwise update the
game at every container start and move it off the build the plugin was made for. If you host the
server some other way, turn automatic game updates off there too.

### 4. Check that it worked

The server console (and `<server folder>/takaro/plugin.log`) shows, within about 20 seconds of the start:

```
[Takaro] connecting to wss://connect.takaro.io/ as "<name>"
[Takaro] connected to Takaro as "<name>"; the server shows as reachable in the Takaro dashboard
```

and the game server turns **online** in Takaro. A block of `*` lines says what to fix and which file to
edit: `registrationToken not set` means the token is missing, `Takaro refused this server` means it is
wrong, `Takaro refused the server name` means another game server in your Takaro domain already has
that name.

### 5. Upgrading

Stop the server, replace only `dbghelp.dll` with the new one, start it again. Your config in
`takaro/plugin.json` and the connector's state in `takaro/connector-state/` stay as they are.
`takaro/enshrouded-gamedata.json` is yours too: a newer plugin uses its own embedded copy for any build
your file has no section for, so delete the file to pick up the new defaults for every build.

**From 0.5.0 (plugin + sidecar):** 0.6.0 replaces the sidecar. Stop and remove the sidecar before the
new DLL starts, so only one of them connects with the server's identity; move the tokens from
the sidecar's settings into `takaro/plugin.json` (or the game server's environment), and copy the
sidecar's `event-cursor.json` and `online-players.json` into `takaro/connector-state/` so every event is
sent exactly once. [INSTALL.md](INSTALL.md) (also inside the zip) has the exact steps and the rollback to
0.5.0.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
