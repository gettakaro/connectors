# Valheim connector acceptance validation — 2026-09-02

## Scope

First run of the Valheim connector against the reusable Takaro connector
acceptance checklist (`docs/connector-testing/CHECKLIST.md` in the connector
research workspace). Earlier Valheim ledgers predate that checklist, so its
`PRE`/`MOD`/`RES`/`DATA` sections had never been exercised for this game.

The governing rule of that checklist applies here: a connector is accepted on
what it does *through Takaro*, not on what its own code or logs claim. Every
verdict below cites a Takaro-visible result. `SKIP` means not verified and is
never reported as a pass.

## Candidate identity

- Connector source: `origin/main` @ `ab16dfe` (valheim `version.txt` = `2.0.0`)
- Deployed plugin and companion version: `2.0.1`
- Companion protocol: `1`
- Deployed server DLL SHA-256:
  `a7770141749c07680e9cc8f2ef1f2f1a0a1bea30525df2b925036d2bcd00e2a7`
- Deployed client DLL SHA-256:
  `7bd3fb79154f73cf699ba1cdf2d14bb67d613a5d2434d70bd29cba09a72efcb6`

Both deployed DLL hashes match the 2026-07-14 ledger exactly, so this run
exercises the same artifact that ledger validated, with no rebuild.

## Environment

- Takaro API: `https://api.takaro.io`, domain `shiny-bats-bake`
- Takaro game server: `82f53af5-bb18-4c15-98bf-6fb956d433e1` ("Takaro Dev Valheim")
- MCP: local `takaro-mcp` on `127.0.0.1:4000`, 215 tools (matches `openapi.json`)
- Server config: `companionMode = required`, `commandAllowlistExact = help`

### Identity-binding correction

The deployed configuration identified as game server
`4dadfdf6-18a3-41f1-ae2c-b94200dea9ab`, which does not exist in domain
`shiny-bats-bake`; that id belongs to a different domain. The registration token
in the workspace `.env` also resolved to domain `bumpy-mangos-kick`, so
identification failed with HTTP 409 while Takaro tried to create a duplicate
game server.

Rebinding required both values to come from the target domain: `identityToken`
was set to the existing server's `takaro-dev-valheim`, and `registrationToken`
to that domain's `serverRegistrationToken`. The connector then identified as
`82f53af5-…`. Note that the connector refuses to start with an empty
`registrationToken` even when the game server already exists.

## Static baseline

`dotnet test valheim/Takaro.Valheim.sln` → **417 passed, 1 failed** (418 total).

- `ReleasePackageContractTests.ValidSeparateServerAndClientFixturesPass` initially
  failed with `required package validation command is missing: rg`. Installing
  ripgrep resolved it; CI installs it explicitly, so this is a local-environment
  requirement, not a defect.
- `CapabilityRegistryTests.ReadmeDoesNotAdvertiseMissingValheimJustRecipes`
  **fails on a clean `origin/main` checkout**. The test asserts the root
  `justfile` does not contain `build-release-valheim `, but `justfile:39` defines
  exactly that recipe. Pre-existing upstream defect, unrelated to this run.

## Verdicts

### Connection

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| CONN-1 | Gameserver record exists | PASS | `gameserverGetOne` → "Takaro Dev Valheim", type `GENERIC` |
| CONN-1b | Gameserver enabled | PASS | `enabled: true` |
| CONN-4 | Reachability true in Takaro record | PASS | `gameserverGetOne` → `reachable: true` |
| GAP-0 | API surface matches spec | PASS | 215 operations, matches `openapi.json` |
| PRE-6 | Economy enabled | PASS | `economyEnabled: true` |

### Actions

| ID | Action | Verdict | Evidence |
| --- | --- | --- | --- |
| ACT-1 | `testReachability` | PASS | `gameserverTestReachabilityForId` → `{"connectable": true}` |
| ACT-2 | `getPlayers` | PASS | `gameserverGetPlayers` → `[]` with runtime available |
| ACT-9 | `getMapInfo` | PASS (unsupported, honestly) | Payload error `server_only_unsupported: Valheim dedicated servers do not expose…`; no fabricated data |
| ACT-14 | `executeConsoleCommand` | PASS | `help` → `{"success": true, "rawResult": "Executed allowlisted Valheim console command: help"}` |
| ACT-14b | `executeConsoleCommand` allowlist denial | PASS | `help players` → `{"success": false, "rawResult": "command_not_allowed: …"}` |
| ACT-19 | `listBans` | PASS | `gameserverListBans` → `[]` (bare array, not an error object) |

### Non-fabrication under an unready world

Before Valheim finished loading its world, the connector logged
`suppressed unsupported failure response: action=getPlayers, error=runtime_unavailable`
and `lifecycle polling skipped because the server player list is unavailable
(runtime_unavailable); existing lifecycle state is preserved`.

This is the documented behavior working as intended: rather than returning `[]`
for an array-validated action and inventing an empty server, the connector lets
Takaro expire the request, and lifecycle polling preserves its prior snapshot
instead of emitting false disconnects.

## Live client session

A real graphical client joined with the owned companion installed:

```text
client   Loading [Takaro Valheim Companion 2.0.1]
client   Takaro Valheim Companion initialized for protocol 1.
client   Takaro Valheim Companion chat hooks initialized with 1 command prefix(es).
client   Takaro Valheim Companion negotiated protocol 1 with the connected server.
server   Got handshake from client 76561198000735875
server   Got character ZDOID from Hehe : 675679570:2
server   Takaro Valheim companion hello sent to peer 675679570.
```

`gameserverGetPlayers` then returned the player with the required identity shape:
`gameId=Steam_76561198000735875`, `name=Hehe`, `platformId=steam:76561198000735875`.

### Companion-backed capabilities

| ID | Capability | Verdict | Evidence |
| --- | --- | --- | --- |
| ACT-12 | `sendMessage` | PASS | Takaro request → server `response frame written: action=sendMessage, success=True`; server logged `server message routed to 1 peer(s); skipped 0 peer(s) without compatible companion chat`; client logged `rendered a server message from Takaro in chat` |
| ACT-5 | `getPlayerInventory` | PASS | Takaro polled repeatedly; `response frame written: action=getPlayerInventory, success=True`. The test character's inventory was genuinely empty, which the companion reports as a *confirmed* empty snapshot rather than the server fabricating `[]` |
| EVT-chat-message | `chat-message` | PASS | Player typed `acceptance plain chat 0902`; Takaro `eventSearch` persisted it bound to `playerId 44ad4e7a-…` |

`chat-message` payload completeness (EVT payload section):

```json
{
  "player": {
    "gameId": "Steam_76561198000735875",
    "name": "Hehe",
    "steamId": "76561198000735875",
    "platformId": "steam:76561198000735875"
  },
  "channel": "global",
  "timestamp": "2026-09-02T07:55:56.466Z",
  "msg": "acceptance plain chat 0902"
}
```

This is the capability that is impossible on a server-only Valheim connector. Vanilla
Valheim routes normal chat only to character owners, so the dedicated server never sees
it. The companion is what makes it observable.

### Module layer

The `teleports` module (version `0.0.4`) was installed on the game server, then driven
entirely from in-game chat:

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| MOD-1 | Module installed | PASS | installation `36720839-1fda-4960-ab44-c2eeadbeee91` |
| MOD-3 | Chat command parsed | PASS | `@tplist` produced `command-executed` for command `tplist` at `2026-09-02T07:56:44.841Z` |
| MOD-7 | Player-visible module reply | PASS | Whisper `You have no teleports available, use @settp <name> to set one.` persisted as a `chat-message` with `channel: whisper`, and the client logged `rendered a server message from Takaro in chat` |
| CHAT-1 | Command prefix does not collide | PASS | prefix `@` |
| EVT-19 | No chat echo loop | PASS | The player's own `@tplist` input appears once; the module reply is a separate whisper row and is not re-emitted as player-originated chat |

This is the complete end-user loop — player types a command in Valheim, Takaro executes
a module, and the reply is rendered back in the player's chat — and it depends on the
companion at both ends.

### Server-owned actions with a live player

| ID | Action | Verdict | Evidence |
| --- | --- | --- | --- |
| ACT-4 | `getPlayerLocation` | PASS | `playerongameserverSearch` persisted real non-origin coordinates `75, 36, 3` with `online: true` |
| ACT-11 | `giveItem` | PASS | `dropped 1x Wood for Hehe (Steam_76561198000735875) at x=74.56177, y=35.93914, z=2.84836`; the dropped item is visible in-world |
| ACT-15 | `teleportPlayer` | PASS | `routed base-game teleportPlayer to Hehe: x=80, y=36, z=5` |
| DATA-3 | Coordinates round-trip | PASS | After the teleport, Takaro read the position back as exactly `80, 36, 5` |
| ACT-17 | `banPlayer` | PASS | `Steam_76561198000735875` written to Valheim `bannedlist.txt`; player disconnected; **the headless server stayed alive**, confirming the `Kicked`-RPC path avoids the known `ZNet.Disconnect(peer)` crash |
| ACT-19 | `listBans` (populated) | PASS | `[{"player":{"gameId":"Steam_…","name":"Hehe"},"reason":"","expiresAt":null}]` |
| ACT-18 | `unbanPlayer` | PASS | Ban removed from `bannedlist.txt`; `listBans` returned `[]` |

Ban state was left clean: `bannedlist.txt` contains only its header comment.

### Defect found: ban reason is dropped

`gameserverBanPlayer` was called with `reason: "acceptance test"`. Takaro's own
`listBans` read the ban back as `"reason": ""`. Valheim's native ban list stores only a
player identifier per line, so the connector has nowhere to persist the reason, and it
is silently discarded rather than reported as unsupported.

### Lifecycle events

| ID | Event | Verdict | Evidence |
| --- | --- | --- | --- |
| EVT-player-connected | `player-connected` | PASS | `2026-09-02T07:53:48.803Z`, bound to `playerId 44ad4e7a-…` |
| EVT-player-disconnected | `player-disconnected` | PASS | `2026-09-02T07:58:11.307Z`, emitted when the ban disconnected the player |

Takaro-derived events observed in the same window: `player-created`,
`player-new-name-detected`, `player-sync-snapshot`, `server-status-changed`,
`module-installed`, `command-executed`.

## `companionMode` behaviour

Each mode was tested by restarting the dedicated server with that value.

### `disabled`

The server started with **no companion log lines at all** — the companion RPC is never
registered, exactly as documented.

`sendMessage` in this mode returned an immediate, honest failure rather than silently
doing nothing:

```text
companion_server_chat_unavailable: No ready Valheim peer has an active compatible
Takaro companion chat session.
```

The connector logged `response frame written: action=sendMessage, success=False`.

### `required`

The server registered the companion RPC and sent `companion hello` to the connecting
peer. With a companion-equipped client this completed as
`negotiated protocol 1 with the connected server`.

With a **vanilla client** (companion folder removed from
`BepInEx/plugins`, BepInEx type-loader cache cleared), `required` mode behaved exactly
as `COMPANION.md` describes:

```text
10:02:15  Got character ZDOID from Hehe : 925057088:2
          Takaro Valheim companion hello sent to peer 925057088.   (x3, unanswered)
10:02:4x  Takaro Valheim required companion enforcement scheduled for peer 925057088:
          reason=MissingCompanion, expected=1, actual=missing.
          Takaro Valheim sent the built-in kicked RPC to peer 925057088 after the
          companion explanation grace period.
10:02:47  Closing socket 76561198000735875
```

The client logged `Lost connection to server:ErrorKicked`, `getPlayers` dropped from
`1` to `0`, and **the headless dedicated server stayed alive**. The enforcement path
uses Valheim's built-in `Kicked` RPC rather than a direct `ZNet.Disconnect(peer)`, which
is the documented reason it does not crash the server.

### Vanilla-client degradation, observed

While the vanilla client was connected but before enforcement fired, the connector
demonstrated its non-fabrication guarantees:

```text
getPlayerLocation has no real server-observed position for 'Steam_76561198000735875'.
response frame written: action=getPlayerLocation, success=False.

suppressed unsupported failure response: action=getPlayerInventory,
error=player_component_unavailable. The Generic Connector has no compatible failure
payload for this action; Takaro will expire the pending request instead of accepting
fabricated state.
```

`getPlayers` still worked and returned the player. So on a vanilla client the
server-owned surface keeps working and only the client-owned surface degrades — and it
degrades by refusing to answer rather than by inventing an empty inventory.

The kicked client showed Valheim's own modal dialog reading
**"You have been kicked from the server."**, confirming the disconnect is
player-visible and not a silent drop.

### `optional`

With the same **vanilla client**, `optional` mode registered the companion RPC, sent
`companion hello`, received no answer, and **took no action**:

```text
10:16:39  Got handshake from client 76561198000735875
10:16:56  Got character ZDOID from Hehe : 478968791:2
10:18:29  getPlayers returned 1 player      (still connected, ~93s after joining)
```

Zero `enforcement scheduled` and zero `kicked RPC` lines were emitted in the whole
session. A vanilla client is left alone in `optional` mode, three times past the
30-second grace window that terminates it under `required`.

### Mode summary, as observed

| Mode | Companion RPC registered | Vanilla client | Client-owned capabilities |
| --- | --- | --- | --- |
| `disabled` | No | Joins and stays | None; `sendMessage` fails with `companion_server_chat_unavailable` |
| `optional` | Yes | Joins and stays | Available only from players who installed the companion |
| `required` | Yes | Disconnected after ~30s grace, with a visible "You have been kicked from the server." dialog | Available from every player, because every player must have one |

## Moderation and destructive actions

| ID | Action | Verdict | Evidence |
| --- | --- | --- | --- |
| ACT-16 | `kickPlayer` | PASS | `kicked Hehe (Steam_76561198000735875). Reason: acceptance kick test.`; `getPlayers` dropped to `0`; server stayed alive |
| ACT-17 | `banPlayer` | PASS | See the moderation section above |
| ACT-18 | `unbanPlayer` | PASS | See the moderation section above |

`kickPlayer` carries its reason into the server log, while `banPlayer` cannot persist
one. Both use the built-in `Kicked` RPC and neither crashed the headless server.

## Item and entity catalog

The first automated pass reported `listItems` as FAIL with an empty catalog. That was a
**Takaro-side sync-cadence artifact, not a connector defect**: at that point Takaro had
never issued a `listItems` request, which the connector log confirmed
(`action=listItems` count was `0`).

Triggering the sync jobs explicitly (`gameserverTriggerJob` with `type: syncItems` and
`syncEntities`) produced real data immediately:

```text
listItems returned 821 item prefab(s).      → Takaro itemSearch total: 821
listEntities returned 101 character prefab(s). → Takaro entitySearch total: 101
```

| ID | Action | Verdict | Evidence |
| --- | --- | --- | --- |
| ACT-6 | `listItems` | PASS | 821 items persisted in Takaro (`item_tin`, `item_sap`, `item_bow`, …) |
| ACT-7 | `listEntities` | PASS | 101 entities persisted in Takaro (`enemy_bat`, `enemy_lox`, `enemy_hen`, …) |

Worth noting for operators: a freshly registered Valheim game server shows an empty item
catalog until Takaro's sync job runs, and an empty catalog blocks `giveItem` autocomplete
and shop listings. It is not a connector fault and does not need a reinstall.

| ID | Action | Verdict | Evidence |
| --- | --- | --- | --- |
| ACT-20 | `shutdown` | PASS | `response frame written: action=shutdown, success=True` **before** `executing scheduled shutdown on the Unity main thread`; Valheim logged `ZNet Shutdown` and the process exited cleanly |

`shutdown` was run last, as the checklist requires, because it ends the session.

## Registry changes made from this run

| Action | Was | Now | Why |
| --- | --- | --- | --- |
| `kickPlayer` | `unsupported` | `live-supported` | Live-proven; server survived |
| `banPlayer` | `unsupported` | `live-supported` | Live-proven; written to and removed from Valheim's ban list |
| `unbanPlayer` | `unsupported` | `live-supported` | Live-proven |
| `shutdown` | `unsupported` | `live-supported` | Live-proven; clean exit |

Ownership correction: `sendMessage` moved from `server-owned` to `client-reported`,
because `companionMode=disabled` makes it fail with `companion_server_chat_unavailable`.
Delivery genuinely depends on a companion on the receiving client.

`getPlayer` remains `unsupported`: Takaro's MCP surface exposes no single-player getter,
so its response shape could not be proven through Takaro in this run. `listLocations`
remains `schema-fallback` for the documented upstream reason.

## Not verified in this run (SKIP)

These are recorded as SKIP, not as passes:

- `getMapTile` — needs tile coordinates; the action is declared `unsupported` anyway.
- `player-death` and `entity-killed` — the test character had no weapons and the client
  was launched without `-console`, so no controlled kill or death could be staged. Both
  remain `live-supported` in the registry on the 2026-07-12 companion ledger's evidence;
  this run neither re-proved nor contradicted them.
- RES-1/2/4/5/7 (restart and log-rotation resilience) — the session ended with the
  `shutdown` test.
- ECON-1..19 shop and currency flows.

## Result

The connector performed correctly on every capability exercised. The two failures the
automated pass reported were both resolved without a code change: the empty item catalog
was a Takaro sync-cadence artifact, and the missing module was a test-environment gap.

One real defect was found: **`banPlayer` silently discards the ban reason**, because
Valheim's ban list format cannot store one.

Under the checklist's own scoring rule, unverified items are SKIP and a SKIP in the
required set means "not yet accepted". Several RES and ECON items were not reached, so
this run does not by itself declare the connector accepted. What it does establish is
that the deployed 2.0.1 artifact's action surface, event surface, module loop, and all
three companion modes behave as documented, with four previously unproven actions now
carrying exact live evidence.
## Economy, tracking, and restart — second session

The first session ran `shutdown` before reaching the ECON, DATA, and RES sections. The
server was restarted and those sections were then worked properly.

### Restart resilience

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| RES-1 | Game server restart — reconnects | PASS | After restart the connector re-identified as `82f53af5-…` without config changes |
| RES-2 | Game server restart — events resume | PASS | A fresh `player-connected` was persisted at `2026-09-02T08:46:42.553Z`, after the restart; `chat-message`, `command-executed`, and inventory-change events all continued to flow in the new session |

### Currency

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| PRE-6 | Economy enabled | PASS | `economyEnabled: true`, currency name "Takaro coins" |
| ECON-2 | Add currency | PASS | `playerongameserverAddCurrency` 500 → balance read back as `500` |
| ECON-3 | Deduct currency | PASS | Deduct 200 → balance read back as `300` |

### Shop

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| ECON-5 | Create shop listing | PASS | Listing `bd384176-…` "Acceptance Wood Bundle", price 100, 5x Wood, `draft: false` |
| ECON-6 | Shop listing search | PASS | `shoplistingSearch` returned the listing bound to this game server |
| ECON-7 | Place shop order | **SKIP** | `shoporderCreate` returns `Unknown player, make sure you have linked your account`, and `playerGetMe` returns `Player not found, please link your player account.` Orders are placed *as a player*, and this admin session has no linked player. Linking needs an in-game code redeemed through `playerOnboarding`. Not a connector defect — the order never reaches the connector. |
| ECON-16 | Shop actions available | N/A | `shopactionGetAvailable` returned `[]`; listings on this server can only deliver items |

### Shop delivery is a world drop — operator-facing caveat

Shop claims deliver through `giveItem`, and on Valheim `giveItem` **spawns items on the
ground**, it does not insert into a player's inventory. A 5x Wood delivery logged:

```text
dropped 5x Wood for Hehe (Steam_76561198000735875) at x=79.85327, y=36.01536, z=4.947341.
```

A screenshot taken immediately afterwards showed the player's inventory bar still empty,
with the stack lying at their feet. This is inherent to the server-only boundary — the
dedicated server cannot write into a remote client's inventory — but it has real
consequences for a Valheim economy that are worth stating before anyone runs a shop:

- Purchased goods are lootable by any nearby player until the buyer picks them up.
- A purchase made while the buyer is falling, swimming, or in transit can be lost.
- A buyer with a full inventory has no feedback beyond the items remaining on the ground.

### Tracking

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| ECON-9 | Inventory history | PASS | 3 inventory snapshots in 24h, each carrying both `name` and `code` (`item_wood` / `Wood`) |
| ECON-12 | Radius query | PASS | A 200-unit radius around `80,36,5` returned the player; a 50-unit radius around `9000,36,9000` correctly returned `[]`. The negative case was checked explicitly. |

### Module state

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| MOD-3 | Chat command parsed **with arguments** | PASS | `@settp spot2` produced `command-executed` with `arguments: {"tp": "spot2"}` |
| MOD-6 | Module variable persisted | PASS | `variableSearch` returned `tp_spot2 = {"name":"spot2","x":80,"y":36,"z":5,"dimension":"valheim"}` — the module wrote real game coordinates |

An automated pass reported MOD-3 as FAIL with "arguments are EMPTY". That was a false
positive: the only command sampled at the time was `tplist`, which legitimately takes no
arguments. Running a command that does take one settled it.

### Final automated pass

`verify-connector.mjs` after this session: **32 PASS / 2 FAIL / 49 SKIP** (up from
9 PASS at baseline). Both remaining FAILs are explained above — MOD-3 was a false
positive now disproven, and `EVT-23 server-status-changed` recorded the connector going
offline during this run, which was the deliberate `shutdown` test.
## Correction — items previously recorded as SKIP

Two items were recorded as SKIP above on reasoning that turned out to be wrong. Both were
re-attempted and one of them resolved.

### Shop purchase: SKIP was wrong, it is PASS

The earlier SKIP said orders cannot be tested because they are placed as a linked player
account. That is true of the **API** route (`shoporderCreate` needs a linked player, and
`playerGetMe` returns "please link your player account"), but it is not the route a
player actually uses. Installing `economyUtils` on the game server exposes the in-game
`shop` command, and a player buys through chat with no account linking at all.

Full purchase driven from inside Valheim:

```text
player   @shop 1 1 buy
Takaro   You have purchased Acceptance Wood Bundle for 100 Takaro coins.
Takaro   You have received items from a shop order.
Takaro   5x item_wood
server   dropped 5x Wood for Hehe (Steam_76561198000735875) at x=79.47, y=36.00, z=4.84
```

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| ECON-4 | Balance command | PASS | `@balance` → `"balance: 300 Takaro coins"`, matching the API |
| ECON-7 | Place shop order | PASS | Order `COMPLETED`; currency went `300 → 200` for a 100-coin listing |
| ECON-8 | Shop claim delivers | PASS | 5x Wood delivered and confirmed in chat |

This also confirms the world-drop caveat on the real purchase path, not just on a
synthetic `giveItem` call: the goods a player buys land on the ground at their feet.

Note for anyone repeating this: modules must be installed against the **correct game
server**. The installation record's field is `gameserverId` (lowercase `s`); filtering on
`gameServerId` silently matches nothing and makes commands look broken.

### `player-death`: SKIP was wrong, it is PASS

The earlier SKIP claimed a controlled death could not be staged without `-console`. It
can — Valheim's built-in `/die` chat command needs no console. Triggering it produced:

```text
server  observed routed OnDeath packet but did not emit an event because routed identity
        and state are not server-owned: sender=208696540, targetZdo=208696540:2.
```

and then the trusted companion report produced the actual Takaro event:

```json
{
  "player": { "gameId": "Steam_76561198000735875", "name": "Hehe",
              "platformId": "steam:76561198000735875" },
  "timestamp": "2026-09-02T09:07:16.635Z",
  "position": { "x": 79.40, "y": 36.00, "z": 4.83, "dimension": "valheim" },
  "msg": "None"
}
```

Those two log lines together are a precise demonstration of the trust boundary: the
dedicated server *sees* the routed death packet and deliberately refuses to emit on it,
because routed client identity is not server-owned. The event comes from the companion's
peer-bound report instead.

### `entity-killed`: still SKIP, and here is exactly why

Not proven in this run. What was tried:

- `executeConsoleCommand` with `spawn Greyling` — the connector allowlisted and forwarded
  it correctly, but Valheim answered `'spawn' is not valid in the current context`.
  `spawn` is a client devcommand; a dedicated server console cannot run it.
- Roaming Meadows to find natural wildlife — the session had rolled into night and the
  test character had lost its weapons to the death test above (they stay in the
  gravestone), so no kill could be staged before this run ended.

It needs a client launched with `-console` and `devcommands` enabled, or a player who
walks into a boar in daylight. It remains `live-supported` in the registry on the
2026-07-12 companion ledger's evidence, which this run did not contradict.

### Event types persisted by Takaro during this run

```
chat-message 24   player-connected 6    player-inventory-changed 10
command-executed 5    player-disconnected 4   player-sync-snapshot 6
player-death 2    currency-added 1      currency-deducted 2
shop-listing-created 1    shop-order-created 1    shop-order-status-changed 1
cronjob-executed 6    hook-executed 1       module-installed 4
```
