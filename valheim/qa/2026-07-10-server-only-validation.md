# Valheim Server-Only Validation Ledger

## Verdict: PASS WITH GAPS (turn-3 runtime; turn-4 pending)

Source commit `20b505b2fcc5e58a6bdb0ec3bf4d26bda6a5f096` passed build/package gates and a live dedicated-server run with a vanilla player. Player actions, catalogs, visible messaging, and cron delivery were proven. Client-owned inventory remains a schema fallback; inbound chat, death, and entity-killed remain unsupported; lifecycle frames were written but exact Takaro persistence was not proven.

Turn 3 then deployed source commit `d0195b677c43a766daae55a226be4af73ef24a10`. It removed schema validation errors and re-proved the core server-only path, including four persisted lifecycle events, but exposed the false-success fallback defect for unavailable inventory/location and a flat `listLocations` DTO. Those failures are recorded below rather than folded into a success claim.

Turn-4 source is not live-validated by this ledger. Its consumer-contract, no-fabricated-inventory, position-cache, and nested-location corrections must be rebuilt, deployed, and exercised before inheriting a live verdict.

## Turn-3 Artifact-Pinned Evidence

### Artifact and boundary

| Evidence | Exact value | Result |
| --- | --- | --- |
| Source commit | `d0195b677c43a766daae55a226be4af73ef24a10` | Exact clean turn-3 HEAD |
| Release zip SHA-256 | `6273a722a98b1685bc87f22c5c4d1338c00ed28ea29e0b2c4ab1eae6d3d7a458` | Deployed build input |
| Packaged/deployed DLL SHA-256 | `8d7818ae0642af9ec6f6e4e67acc236d79957c47d321d1e0dbf0e4da8777b567` | Exact match |
| Evidence directory | `/tmp/valheim-turn3-live-20260710T132532Z` | Local logs, probes, screenshots, and cleanup records |

The client Takaro DLL was absent. The exact plugin identified as game server `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab`; reachability passed. The vanilla player joined twice without a Takaro client plugin. All temporary server, client, MCP, and module state was cleaned up after the run.

To replay against the same source rather than the current worktree, create a detached worktree first:

```bash
git worktree add --detach /tmp/connectors-valheim-turn3 d0195b677c43a766daae55a226be4af73ef24a10
cd /tmp/connectors-valheim-turn3
dotnet test valheim/Takaro.Valheim.sln --no-restore -v minimal
bash valheim/tests/setup-environment-behavior.sh
```

### Actions and DTO findings

- Global/direct messages, visible world-drop `giveItem`, the strict fractional/oversized amount bounds, teleport, allowlisted `help`, and `listBans` passed.
- Fresh catalogs returned `listItems`: 821 and `listEntities`: 101.
- The raw `listLocations` harness returned 11,293 server locations in a 1,528,611-byte response. The server source path was real, but each location used the wrong flat DTO; turn 4 changes this to required nested `position` and still needs live revalidation.
- Schema validation errors disappeared for unavailable inventory/location. That was only a partial fix: Takaro resolves `payload` and ignored the root failure metadata, so `[]` and zero/`unavailable` were persisted as state. This is the confirmed false-success fallback defect, not a pass.
- Destructive actions (`kickPlayer`, `banPlayer`, `unbanPlayer`, `shutdown`) remained skipped and approval-gated.

### Persisted lifecycle proof

Two complete vanilla connect/disconnect cycles persisted four Takaro events:

- `player-connected`: `e51c2951-ec59-4c1b-9be5-8eca3653a7f8`
- `player-disconnected`: `20897cd3-d833-4094-a68c-dfa4c6cf7f12`
- `player-connected`: `bdb561c6-3d43-4a88-8f64-4d6e224e916d`
- `player-disconnected`: `ba9b7643-923c-4a5a-bb0d-7739fe90a6e9`

This upgrades lifecycle persistence itself to live-supported. Connector logs remain deliberately narrower: `lifecycle frame written` proves a transport write only; the event IDs above are the independent persistence proof.

### Module automation

Installed modules were `teleports`, `Waypoints`, and `serverMessages`. A fresh `serverMessages` cron reached the connector, rendered visibly, and persisted `cronjob-executed` event `f55c8b39-fc2c-442f-a4e2-be81a7851f4e`.

A temporary hook persisted successful `hook-executed` event `5ea168d7-1ae1-4d73-a7dc-5731b02957e5`; its temporary module was then uninstalled and the post-cleanup search returned zero. Corrected `/waypoints` routing reached connector `sendMessage`, but command-executed analytics remained at zero, so command-module completion is not claimed.

## Turn-2 Runtime Boundary

- Architecture: dedicated-server-only BepInEx plugin; no client DLL, bridge, snapshot, or custom Takaro RPC.
- Game server ID: `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab`.
- Live window: `2026-07-10T14:35:34+02:00` through `2026-07-10T14:51:25+02:00`.
- Client proof: vanilla `isModded:false`; no `TakaroValheim.dll` under the client BepInEx plugin tree.
- Primary surfaces: Takaro MCP actions/searches, dedicated-server BepInEx log, and player-visible screenshots. Secrets and raw authenticated payloads are intentionally omitted.

## Reproducible Artifact Identity

| Artifact | SHA-256 | Result |
| --- | --- | --- |
| Release zip | `4142c2399a660bbda32200e1e18e79e75bb1d3f5b478cf8387681b9a80c1d1ac` | PASS: built from commit `20b505b` |
| DLL inside release zip | `0a70626f6908669846b8bbfc2d2aa93e44a5902dccc44fba259bb6d0f5c505cc` | PASS |
| Deployed server DLL | `0a70626f6908669846b8bbfc2d2aa93e44a5902dccc44fba259bb6d0f5c505cc` | PASS: exact artifact match |

Replayable build commands:

```bash
git worktree add --detach /tmp/connectors-valheim-turn2 20b505b2fcc5e58a6bdb0ec3bf4d26bda6a5f096
cd /tmp/connectors-valheim-turn2
dotnet test valheim/Takaro.Valheim.sln --no-restore -v minimal
bash valheim/tests/setup-environment-behavior.sh
dotnet build valheim/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -f net472 -p:EnableValheimPluginBuild=true \
  -p:BepInExReferencePath=/home/hendrik/valheim-dedicated-server/BepInEx/core \
  -p:ValheimReferencePath=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed
VALHEIM_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed \
BEPINEX_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/BepInEx/core \
bash valheim/scripts/build-release.sh 0.0.0-dev /tmp/valheim-turn2-release
```

## Timestamped Runtime Trace

| Time (Europe/Brussels) | Observation | Verdict |
| --- | --- | --- |
| `14:35:34` | Connector transport handshake completed and the dedicated server identified to Takaro. | PASS |
| `14:40:14` | Vanilla player `Hehe` was mapped and the server snapshot produced a player-connected frame. | PASS at server boundary; Takaro persistence unproven |
| `14:40:26` | Valheim supplied the player character ZDO; player-bound actions became ready. | PASS |
| `14:51:25` | Vanilla client disconnected; server player count returned to zero and a player-disconnected frame was written. | PASS at server boundary; Takaro persistence unproven |

`getPlayers`: `0 -> 1 -> 0`. `getPlayerLocation` succeeded while the player was ready. Before the character position was ready and after disconnect, commit `20b505b` returned schema-invalid error objects; this ledger records that defect rather than treating the failures as position proof.

## Action Coverage

| Action | Evidence | QA result |
| --- | --- | --- |
| `testReachability` | MCP returned `connectable:true`. | PASS |
| `getPlayers` | MCP observed `0 -> 1 -> 0` across join/disconnect. | PASS |
| `getPlayerLocation` | Live non-origin position succeeded after character readiness. | PASS for ready player; unavailable wire shape required turn-3 correction |
| `getPlayerInventory` | Dedicated server had no remote `Player` inventory; repeated Takaro errors said `Expected array ... got object`. | SCHEMA-FALLBACK REQUIRED |
| `sendMessage` | Global and direct messages rendered in the vanilla client. | PASS |
| `giveItem` | Wood x1 created a real world drop that the player picked up; amount `1001` returned `success=False` and created no extra drop. | PASS |
| `teleportPlayer` | Server-owned position changed `135,33,-2 -> 140,33,-2` through base-game teleport. | PASS |
| `listItems` | Takaro catalog search contained `821` items. | PASS |
| `listEntities` | Takaro catalog search contained `101` entities. | PASS |
| `listLocations` | BLOCKED: no current Takaro MCP route exposed this action during the run. | BLOCKED, not grouped with the proven catalogs |
| Moderation and shutdown | Destructive actions (`kickPlayer`, `banPlayer`, `unbanPlayer`, `shutdown`) were not run against the non-disposable player/server. | SKIPPED, approval-gated |

Exact persisted catalog totals were `listItems`: 821 and `listEntities`: 101. `listLocations`: BLOCKED because the current MCP surface had no route.

## Event and Transport Evidence

The server log recorded player-connected and player-disconnected lifecycle frames, but the exact post-disconnect Takaro MCP `eventSearch` window returned zero persisted lifecycle events. The same server/window query returned other events, so send-attempt logs are not persistence proof. The connector transport has no positive game-event acknowledgement; lifecycle stays `schema-fallback` until a post-unavailable-shape-fix live run returns the records from Takaro.

- `chat-message`: UNSUPPORTED; vanilla inbound chat has no trusted dedicated-server route.
- `player-death`: UNSUPPORTED; routed packet identity/death state is not trusted server-owned proof.
- `entity-killed`: UNSUPPORTED; the accepted server-only branch does not emit it.

## Module Automation

The live run proved two `serverMessages` cron deliveries: both reached `sendMessage`, were routed to one peer, rendered visibly, and had Takaro `cronjob-executed` persistence. This proves scheduling, connector delivery, and player-visible output.

The pre-connector `commandTrigger` probe returned `404`; this is recorded as an external module/API route failure, not connector command proof. No hooks were installed, so hook execution is BLOCKED and not claimed.

## Visual Evidence

- Global/direct delivery: `/tmp/valheim-turn2-visible-direct-window.png`
- World-drop delivery: `/tmp/valheim-turn2-giveitem-visible.png`
- Module cron delivery: `/tmp/valheim-turn2-module-cron-visible.png`

These files are local evidence paths, not committed release assets.

## Final Gate

Build and deploy the turn-4 artifact server-side only. Require: unavailable location rejects without mutating position; inventory times out without persisting `[]`; a ready and freshly disconnected player returns only a real observed position; `listLocations` passes the nested DTO route; persisted lifecycle still works; visible messaging/item/teleport remain intact; the client plugin stays absent; and unsupported chat/death/entity events remain absent. Destructive actions remain approval-gated.
