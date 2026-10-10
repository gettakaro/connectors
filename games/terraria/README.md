# Takaro Terraria Connector

Two server-side pieces (version **0.2.0**) that connect a Terraria dedicated server running
**TShock** to Takaro: a TShock plugin and a bridge service. Players do not install anything.

The plugin alone cannot talk to Takaro, and the bridge alone cannot report deaths, NPC kills,
player coordinates, inventories, or coordinate teleports. Install both.

## Install

Download the latest release: https://takaro.io/connectors/terraria

### 1. Before you start

You need:

- A **Terraria dedicated server running TShock** (Linux or Windows) that you can stop, start and
  copy files to, plus its **`ServerPlugins/` folder** and its **`tshock/` folder**.
- **Node.js 22** on the same host, to run the bridge next to the server.
- A **Takaro account** with a game server created of type **Generic**, and its **registration
  token** (Takaro shows it when you create the game server).

The plugin is compiled against **TShock 6.2.1 for Terraria 1.4.5.8**, using the assemblies
inside `ghcr.io/pryaxis/tshock@sha256:70e59a8e6b4c79b5fad469d320962c1fc98625ed3a955510a2f2641dbff2f7e7`
— the image by digest, never a floating `stable` tag. TShock must match the Terraria server
protocol version, and Terraria clients must match the server — a client newer than the TShock
build is rejected at join time with `You are not using the same version as this server.`

<!-- takaro-maint:targets:begin -->
| Target | Game version | Platform | Loader / API | Java | Support | Verified level |
| --- | --- | --- | --- | --- | --- | --- |
| `tshock-v6.2.1` | v6.2.1 | tshock | — | None | maintained | contract |
<!-- takaro-maint:targets:end -->

### 2. Download

From the latest `terraria-vX.Y.Z` release on the releases page:

> https://github.com/gettakaro/connectors/releases

Download both files. Their names carry the catalog target they were built for:

- **`takaro-terraria-plugin-tshock-v6.2.1-<version>.zip`** — the TShock plugin
- **`takaro-terraria-bridge-tshock-v6.2.1-<version>.zip`** — the bridge service

Direct link pattern:
`https://github.com/gettakaro/connectors/releases/download/terraria-v<version>/takaro-terraria-plugin-tshock-v6.2.1-<version>.zip`

The short names **`takaro-terraria-plugin.zip`** and **`takaro-terraria-bridge.zip`** are on every
release too, byte-identical to the target-named files. They are kept for two releases so existing
links do not break; new instructions should use the target-named ones.

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
`node_modules/`, `package.json`, `package-lock.json`, `TakaroConfig.txt` and two readme files.
Extract it anywhere on the same host. Its one runtime dependency is already in the archive, so
there is nothing to install — run `npm ci --omit=dev` only if you delete `node_modules/`.

### 4. Configure

**TShock REST.** In `<server>/tshock/config.json` set:

```json
"RestApiEnabled": true,
"RestApiPort": 7878
```

Create an application REST token for a TShock user that holds the **`takaro.admin`** permission.
A user in the `superadmin` group already has it through TShock's wildcard. Without
`takaro.admin`, teleport fails, player location reports `0,0,0` and inventory comes back empty —
all silently, without an error. Grant **`tshock.broadcast`** too if you are not using
`superadmin`: with it, a message from Takaro is also written to the server console, which is
where you would look for it. Without it the message still reaches the players.

**Bridge.** Open `TakaroTerrariaBridge/TakaroConfig.txt` and fill in the two tokens:

```
registrationToken=your-registration-token-here
tshockToken=your-tshock-rest-token
```

Leave `identityToken` empty — the bridge fills it in. The other lines already hold working
defaults (`tshockBaseUrl=http://127.0.0.1:7878`, `logFiles=tshock/logs`, ...).

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

Start the server, then start the bridge from inside `TakaroTerrariaBridge/`:

```bash
npm start
```

You can also fill in the tokens while the bridge is running: it checks `TakaroConfig.txt` every
few seconds and connects as soon as you save a token. No restart needed.

### 5. Check that it worked

In the TShock server console / log:

```
Takaro Terraria Events plugin loaded (<version>)
```

In the bridge's own output:

```
Terraria bridge health: http://127.0.0.1:3020/health
Identified successfully with Takaro (gameServerId=...)
```

Then ask the bridge how it is doing:

```bash
curl http://127.0.0.1:3020/health
```

`takaroIdentified` and `tshockReachable` must both be `true`, and `gameServerId` must not be
`null`. `GET /coverage` on the same port lists what each action and event is expected to do.

And in Takaro, the game server shows as **online**.

If no token is set yet, the bridge shows a banner instead:

```
*************************************************************************
  RegistrationToken not set, the server is not connected to Takaro.
  Paste the registration token from Takaro into registrationToken= in /path/to/TakaroTerrariaBridge/TakaroConfig.txt
  and save it. The bridge connects within a few seconds, no restart needed.
*************************************************************************
```

If the token is wrong, the banner says `Takaro rejected identify: Invalid registrationToken
provided`. Fix `registrationToken` in `TakaroConfig.txt` and save; the bridge reconnects within a
few seconds.

### 6. Upgrading

**Stop the server and the bridge first.** Replace
`<server>/ServerPlugins/TakaroTerrariaEvents.dll` with the new one, delete the
`TakaroTerrariaBridge/` folder and extract the new one in its place. Start the server, then the
bridge.

You do not need to enter anything again: the bridge keeps a copy of its settings next to the
folder, in `TakaroTerrariaBridge.saved-config.txt`, and puts them back into the new
`TakaroConfig.txt` on its first start. Your server keeps its identity in Takaro. A
`registrationToken` you put in the new `TakaroConfig.txt` always wins over the copy.

Coming from 0.4.1 or earlier, whose README told you to replace the folder too: back up
`TakaroTerrariaBridge/TakaroConfig.txt` before you delete the folder, and copy it over the new
one. A `TakaroConfig.txt` you keep outside the folder (started with `BRIDGE_CONFIG`, or from that
directory) is never touched by an upgrade.

## What works, what doesn't

On 2026-09-21 the connector was run against a real TShock 6.1.0 server (the pinned image, the
built plugin and bridge): once against a stand-in Takaro, which drove every action below that is
marked proven, and once against Takaro itself, which identified the server and answered. The rows
still marked "not verified in a live test" need a connected player, which that run did not have.
The current target, `tshock-v6.2.1`, was proven the same way by `takaro-maint verify`
(`build`, `startup`, `handshake`, `items`, `entities`, `action`, `references`, `reconnect` and
`shutdown` all pass, reaching level `startup` against the required `contract`); the rig-hosted,
client-adjacent proof below is still the one recorded on 6.1.0 and has not been rerun.
✅ = proven, ⚠️ = works with a caveat or unproven, ❌ = does not work.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | Proven 2026-09-21: the bridge identified to Takaro itself and stayed answering its requests, with TShock reachable. |
| Server restart / reconnect | ✅ | Proven 2026-09-21: the connection was cut and the bridge identified again ~3 s later, then answered normally. |
| Player list | ⚠️ | Read from the TShock REST API. Proven 2026-09-21 on an empty server (an empty list, live); never checked with a player on it. |
| Single player lookup | ⚠️ | Read from the TShock REST API. Not verified in a live test. |
| Player location | ⚠️ | Uses the plugin's `/takaropos` command; checked against a connected player on a local server, but never end to end through Takaro. |
| Player inventory | ⚠️ | The plugin's `/takaroinv` reports inventory, armour, dyes, trash, piggy bank/safe/forge/void vault and stored loadouts. The capability record still lists inventory as returning an empty list, so which behaviour you get is unconfirmed — not verified in a live test. |
| Item catalogue | ⚠️ | 6147 items extracted from the server assemblies, so a name like `Wood` resolves to the code `/give` wants. Proven 2026-09-21: returned in full to a live request, with `Wood` resolving to `9`. The names are split out of the internal ids rather than read from Terraria's own language file, so some of them read wrong — see known issues. |
| Entity catalogue | ❌ | Terraria NPCs spawn from world state; there is no registry to list, so Takaro gets an empty list. |
| Locations / points of interest | ❌ | Terraria has no named-location concept for Takaro to list; Takaro gets an empty list. |
| Chat messages from players | ⚠️ | Parsed out of the TShock log, which is best-effort text matching. Not verified in a live test. |
| Broadcast a message | ✅ | Runs TShock's `/broadcast`, so it reaches the players and the server console. Proven 2026-09-21 against a live server. |
| Whisper a player | ✅ | Runs TShock's `/w`, so only that player sees it (shown as `<From takaro>`). Proven 2026-10-05 against a live server: a module's private reply reached only the player who ran the command. |
| Give an item | ⚠️ | Goes through the plugin so a full inventory is refused rather than dropping items on the floor. Not verified in a live test. |
| Teleport a player | ⚠️ | Uses the plugin's `/takarotp` with world X/Y coordinates. Not verified in a live test. |
| Run a console command | ✅ | Only commands you allowlist run — by default `help` and anything starting with `say` or `time`. Proven 2026-09-21 against a live server. |
| Kick | ⚠️ | Runs the TShock kick command. Not verified in a live test. |
| Ban (timed and permanent) | ⚠️ | The plugin bans the player's UUID **and** their IP and tags the reason `[takaro:<name>]`, because a TShock name ban does not hold against an unauthenticated player. Without the plugin the bridge falls back to the old name ban. Not verified in a live test. |
| Unban | ⚠️ | The plugin finds the ban by its `[takaro:<name>]` tag and clears every identifier. Not verified in a live test. |
| Ban list | ⚠️ | Read from the TShock REST API. Not verified in a live test. |
| Shut the server down | ✅ | Off by default; needs `enableShutdown=true`. Proven 2026-09-21: the server saved and exited cleanly on request. |
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

- **The 2026-09-21 run had no player on the server**, so joining, chat, give, teleport, kick
  and ban are still unproven; the death and NPC-kill events were proven earlier, with a player.
  Treat every ⚠️ row as untested rather than working.
- **Kill weapons can be wrong.** Terraria records no damage source on NPC death, so the weapon is
  whatever the killer was holding when the kill fired — minion, sentry, damage-over-time and
  late-landing projectile kills can credit an item that dealt none of the damage, and it reports
  `unknown` when nothing resolves.
- **Join and leave events are polled, not pushed**, so they lag by up to `pollIntervalMs`.
- **The item names are derived, not Terraria's own.** The catalogue splits each item's internal id
  into words instead of reading the game's language file, so about 120 of the 6147 names glue a
  short word onto the one before it (`A Horrible Nightfor Alchemy`, `Bandof Regeneration`) and
  every apostrophe is gone (`Aarons Helmet`). Giving and looking up still work — the match ignores
  spaces and punctuation, so `A Horrible Night for Alchemy` and `Aaron's Helmet` both find the
  right item — but the names Takaro displays are wrong until the catalogue is regenerated from the
  pinned image's language resource.
- **Takaro only syncs the item list when it feels like it** — on server registration, hourly, or
  on manual trigger — and it skips the sync if the bridge was not attached at registration time.
  Attach the bridge before registering the server, or trigger the job by hand afterwards.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
