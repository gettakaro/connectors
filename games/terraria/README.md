# Takaro Terraria Connector

Two server-side pieces (version **0.2.0**) that connect a Terraria dedicated server running
**TShock** to Takaro: a TShock plugin and a bridge service. Players do not install anything.

The plugin alone cannot talk to Takaro, and the bridge alone cannot report deaths, NPC kills,
player coordinates, inventories, or coordinate teleports. Install both.

## Install

### 1. Before you start

You need:

- A **Terraria dedicated server running TShock** (Linux or Windows) that you can stop, start and
  copy files to, plus its **`ServerPlugins/` folder** and its **`tshock/` folder**.
- **Node.js 22** on the same host, to run the bridge next to the server.
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

The plugin is compiled against the TShock `stable` image (`ghcr.io/pryaxis/tshock:stable`).
TShock must match the Terraria server protocol version, and Terraria clients must match the
server — a client newer than the TShock build is rejected at join time with
`You are not using the same version as this server.`

### 2. Download

From the latest `terraria-vX.Y.Z` release on the releases page:

> https://github.com/gettakaro/connectors/releases

Download both files:

- **`takaro-terraria-plugin.zip`** — the TShock plugin
- **`takaro-terraria-bridge.zip`** — the bridge service

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/terraria-v<version>/takaro-terraria-plugin.zip`

Do not use the `terraria-dev` pre-release or a `pr-<number>-terraria` build; those are untested
rolling builds.

### 3. Copy it into place

Stop the server.

**Plugin.** The zip contains one folder, `TakaroTerrariaEvents/`, holding:

```
TakaroTerrariaEvents/
    TakaroTerrariaEvents.dll
    README.txt
```

Copy **`TakaroTerrariaEvents.dll`** into the TShock server plugin folder — the DLL itself, not the
folder around it:

```
<server>/ServerPlugins/TakaroTerrariaEvents.dll
```

**Bridge.** The zip contains one folder, `TakaroTerrariaBridge/`, holding `dist/`,
`package.json`, `package-lock.json`, `TakaroConfig.example.txt` and two readme files. Extract it
anywhere on the same host, then install its runtime dependency:

```bash
cd TakaroTerrariaBridge
npm ci --omit=dev
```

### 4. Configure

**TShock REST.** In `<server>/tshock/config.json` set:

```json
"RestApiEnabled": true,
"RestApiPort": 7878
```

Create an application REST token for a TShock user that holds the **`takaro.admin`** permission.
A user in the `superadmin` group already has it through TShock's wildcard. Without
`takaro.admin`, teleport fails, player location reports `0,0,0` and inventory comes back empty —
all silently, without an error.

**Bridge.** Copy `TakaroConfig.example.txt` to `TakaroConfig.txt` next to the bridge and fill in:

```
registrationToken=your-registration-token-here
serverName=Terraria Server
takaroWsUrl=wss://connect.takaro.io/
tshockBaseUrl=http://127.0.0.1:7878
tshockToken=your-tshock-rest-token
logFiles=tshock/logs
```

- `registrationToken` — from Takaro. `TAKARO_REGISTRATION_TOKEN` in the environment overrides it.
- `tshockToken` — the REST token from above (`TSHOCK_TOKEN` overrides). Instead of a token you may
  set `tshockUsername` and `tshockPassword`.
- `logFiles` — point at the log **directory**, not a single file. TShock opens a new log on every
  restart; a directory lets the bridge follow that rotation, a single file goes silent after the
  first restart. Chat, join/leave and death/kill events all come through here.
- `enableShutdown` — `false` by default; set to `true` only if you want Takaro to be able to stop
  the server.
- `httpPort` — local status endpoints, default `3020`.

Keep real registration tokens and REST tokens out of version control.

Start the server, then start the bridge:

```bash
npm start
```

### 5. Check that it worked

In the TShock server console / log:

```
Takaro Terraria Events plugin loaded
```

In the bridge's own output:

```
Terraria bridge health: http://127.0.0.1:3020/health
```

Then ask the bridge how it is doing:

```bash
curl http://127.0.0.1:3020/health
```

`takaroIdentified` and `tshockReachable` must both be `true`, and `gameServerId` must not be
`null`. `GET /coverage` on the same port lists what each action and event is expected to do.

And in Takaro, the game server shows as **online**. If it stays offline, the `registrationToken`
in `TakaroConfig.txt` is the first thing to re-check.

### 6. Upgrading

**Stop the server and the bridge first.** Replace
`<server>/ServerPlugins/TakaroTerrariaEvents.dll` with the new one, replace the bridge folder's
`dist/`, `package.json` and `package-lock.json` with the new ones and run `npm ci --omit=dev`
again. Leave your `TakaroConfig.txt` alone — it is not part of either zip and survives the
upgrade. Start the server, then the bridge.

## What works, what doesn't

No live end-to-end test of this connector has been recorded. The statuses below come from the
connector's own capability record and its automated tests, so almost everything is marked
"not verified in a live test" rather than confirmed working.
✅ = proven, ⚠️ = works with a caveat or unproven, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ⚠️ | The bridge connects outbound to Takaro and checks TShock on startup; proven only against a fake TShock, not verified in a live test. |
| Server restart / reconnect | ⚠️ | The bridge reconnects and re-follows the new TShock log after a restart; not verified in a live test. |
| Player list | ⚠️ | Read from the TShock REST API. Not verified in a live test. |
| Single player lookup | ⚠️ | Read from the TShock REST API. Not verified in a live test. |
| Player location | ⚠️ | Uses the plugin's `/takaropos` command; checked against a connected player on a local server, but never end to end through Takaro. |
| Player inventory | ⚠️ | The plugin's `/takaroinv` reports inventory, armour, dyes, trash, piggy bank/safe/forge/void vault and stored loadouts. The capability record still lists inventory as returning an empty list, so which behaviour you get is unconfirmed — not verified in a live test. |
| Item catalogue | ⚠️ | 6147 items extracted from the server assemblies, so a name like `Wood` resolves to the code `/give` wants. Not verified in a live test. |
| Entity catalogue | ❌ | Terraria NPCs spawn from world state; there is no registry to list, so Takaro gets an empty list. |
| Locations / points of interest | ❌ | Terraria has no named-location concept for Takaro to list; Takaro gets an empty list. |
| Chat messages from players | ⚠️ | Parsed out of the TShock log, which is best-effort text matching. Not verified in a live test. |
| Broadcast a message | ⚠️ | Uses the TShock broadcast endpoint. Not verified in a live test. |
| Whisper a player | ⚠️ | Sent per recipient through the same path. Not verified in a live test. |
| Give an item | ⚠️ | Goes through the plugin so a full inventory is refused rather than dropping items on the floor. Not verified in a live test. |
| Teleport a player | ⚠️ | Uses the plugin's `/takarotp` with world X/Y coordinates. Not verified in a live test. |
| Run a console command | ⚠️ | Only commands you allowlist run — by default `help` and anything starting with `say` or `time`. Not verified in a live test. |
| Kick | ⚠️ | Runs the TShock kick command. Not verified in a live test. |
| Ban (timed and permanent) | ⚠️ | The plugin bans the player's UUID **and** their IP and tags the reason `[takaro:<name>]`, because a TShock name ban does not hold against an unauthenticated player. Without the plugin the bridge falls back to the old name ban. Not verified in a live test. |
| Unban | ⚠️ | The plugin finds the ban by its `[takaro:<name>]` tag and clears every identifier. Not verified in a live test. |
| Ban list | ⚠️ | Read from the TShock REST API. Not verified in a live test. |
| Shut the server down | ⚠️ | Off by default; needs `enableShutdown=true`. Not verified in a live test. |
| Player joined event | ⚠️ | Derived by polling the player list, so it arrives up to `pollIntervalMs` (default 10 s) late. Not verified in a live test. |
| Player left event | ⚠️ | Same polling as above, same delay. Not verified in a live test. |
| Player chat event | ⚠️ | See "Chat messages from players". |
| Player death event | ✅ | Reaches Takaro, including who killed the player. Needs the plugin. |
| Entity kill event | ✅ | Reaches Takaro with the NPC and a weapon. Needs the plugin. The weapon is the killer's held item at that moment, not the thing that actually dealt the damage — see known issues. |
| Log events | ⚠️ | The bridge ships TShock log lines, but it must filter its own REST traffic out (`logExcludePatterns`) or it floods Takaro's rate limiter. Not verified in a live test. |
| Map info | ⚠️ | Answers with a disabled map; TShock exposes no map metadata. |
| Map tiles | ❌ | Takaro's API does not support map tiles for Generic-connector servers. |
| Discord chat bridge | ⚠️ | Nothing connector-side blocks it, but it was never tried in a live test. |
| Shop & economy | ⚠️ | Item delivery goes through `giveItem`, which is implemented, but buying and currency were never tried in a live test. |

### Known issues

- **The `terraria-v0.2.0` release has no files attached.** Until a release carries both zips, the
  download links in step 2 only work for a release that does.
- **Nothing here has been proven on a live server end to end** except the death and NPC-kill
  events. Treat every ⚠️ row as untested rather than working.
- **Kill weapons can be wrong.** Terraria records no damage source on NPC death, so the weapon is
  whatever the killer was holding when the kill fired — minion, sentry, damage-over-time and
  late-landing projectile kills can credit an item that dealt none of the damage, and it reports
  `unknown` when nothing resolves.
- **Join and leave events are polled, not pushed**, so they lag by up to `pollIntervalMs`.
- **Takaro only syncs the item list when it feels like it** — on server registration, hourly, or
  on manual trigger — and it skips the sync if the bridge was not attached at registration time.
  Attach the bridge before registering the server, or trigger the job by hand afterwards.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
