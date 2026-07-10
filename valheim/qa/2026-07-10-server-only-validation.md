# Valheim Server-Only Validation Ledger

## Verdict: PASS WITH GAPS (turn-5 artifact/runtime passed; independent Codex review quota-blocked; turn-6 pending)

Turn 5 live-validated source commit `bd4bfff718139d91cb415eece2c1b6b6c763942a` on the dedicated server with no Takaro client plugin. The exact build, vanilla-player boundary, immediate invalid-input errors, inventory non-mutation, and persisted lifecycle passed. The exerciser passed. Independent Codex review did not complete because its exact-turn invocation was quota-blocked until 20:22, so this ledger does not promote the branch to fully verified or release-ready.

Turn-6 source is pending fresh build and live verification. Its action-surface, runtime-availability, world-cache, and atomic setup changes must not inherit turn-5 runtime proof until the orchestrator pins a new commit and artifact.

## Turn-5 Artifact-Pinned Evidence

### Artifact, build, and boundary

| Evidence | Exact value | Result |
| --- | --- | --- |
| Source commit | `bd4bfff718139d91cb415eece2c1b6b6c763942a` | Exact clean turn-5 HEAD |
| Release zip SHA-256 | `b2b1748d266f7281731b992762ef7b3188a720de2224ccddb79745f3a271ac3d` | Deployed build input |
| Packaged/deployed DLL SHA-256 | `3477982857610212a83b006318bd0adea8f861afb7b090179016238a09a1e8b4` | Exact match |
| Unit/contract tests | `145/145` | PASS |
| Setup behavior harness | `13/13` | PASS |
| Real plugin build | real `net472` build: PASS with 0 warnings | PASS |
| Runtime exercise | `Exerciser: PASSED` | PASS |
| Independent review | `Codex review: BLOCKED by quota until 20:22` | BLOCKED, not passed |
| Evidence directory | `/tmp/valheim-turn5-evidence` | Local exact-turn evidence |

Vanilla player `Hehe` connected without a Takaro client plugin. The dedicated-server plugin identified and the safe player-bound/runtime flow completed at the server-only boundary.

### Immediate failures and state integrity

- Oversized `giveItem` amount `1001` returned one immediate HTTP 400 in approximately 400 ms with actionable maximum 1000 detail.
- Fractional amount `1.5` returned one immediate HTTP 400 in approximately 434 ms with expected integer detail.
- Each invalid item request produced exactly one request and one response; neither waited for the pending-request timeout.
- A representative whitespace console failure returned immediately in approximately 891 ms rather than fabricating success.
- The unsupported inventory probe sent 11 inventory requests, produced zero response frames, and recorded no fabricated inventory changes.
- `player-connected and player-disconnected persisted` for the vanilla player during the exact turn-5 run.

## Turn-4 Artifact-Pinned Evidence

### Artifact, build, and boundary

| Evidence | Exact value | Result |
| --- | --- | --- |
| Source commit | `75224f2cc9540f9e40baa6178e4ffb70d247b892` | Exact clean turn-4 HEAD |
| Release zip SHA-256 | `35238e55dd4353374cba26565c2e5daa66de70d5c4d22a5823941d515ea34b6b` | Deployed build input |
| Packaged/deployed DLL SHA-256 | `58e6615b1c078d0f85e86beac9d65eed3d949d3b5e9bf117334421e72db8fb02` | Exact match |
| Unit/contract tests | `121/121` | PASS |
| Setup behavior harness | `8/8` | PASS at turn-4 source |
| Independent Codex review | `Codex review: COMPLETED` | Hard gate completed |

The Takaro DLL remained absent from the client. Vanilla player `Hehe` completed the base-game handshake and received a character ZDO; no client snapshot, Takaro RPC, or client-owned state path was used. Client-launch recovery before that successful join was an external automation concern and is not counted as connector proof or a connector defect.

### Player actions and state integrity

- Unavailable pre-ready location returned required numeric coordinates plus `payload.error`; it did not write a fake origin.
- Unsupported inventory emitted no response and did not write a fake empty inventory.
- The player had a real server-owned location at `80/36/-2`; `teleportPlayer` changed it to `85/36/-2` through the built-in Valheim RPC.
- Global/direct messaging, a valid visible world drop, and teleport were visible in `/tmp/valheim-turn4-visible-actions.png`.
- Invalid `giveItem` amount `1001` and fractional amount `1.5` reached `invalid_args`, but the connector suppressed their frames. Takaro timed out after approximately 10.3 seconds and returned a generic 400. This was the turn-4 release blocker, not a passing negative-path test.

### Lifecycle, catalogs, and modules

- Persisted `player-connected`: `e93ed6d1-29f1-49f7-9bf7-43d4d625f395`.
- Persisted `player-disconnected`: `aee52332-392f-449b-ba92-521ef66b3b71`.
- Raw catalogs returned `listItems`: 821 and `listEntities`: 101.
- The official raw Generic Connector `listLocations` action returned 11,293 nested locations in 1,815,046 bytes. `/tmp/valheim-turn4-evidence/raw-harness-result.json` records that every location used nested `position` and none used flat coordinates.
- Takaro source `0c63cf1c` still throws `NotImplementedError` from the standard Generic `listLocations` route before requesting the connector. The raw action/schema is live-proven, but end-to-end standard routing is not; the capability is therefore `schema-fallback`.
- A visible online `serverMessages` cron persisted `cronjob-executed` event `dd7fabcb-bd18-491c-8ea3-c9d2147be33f`; visual proof is `/tmp/valheim-turn4-module-cron-visible.png`.
- Chat, player-death, and entity-killed remained unsupported with no accepted emitter. Destructive kick/ban/unban/shutdown checks remained approval-gated and skipped.

## Turn-3 Artifact-Pinned Evidence

Turn 3 deployed source commit `d0195b677c43a766daae55a226be4af73ef24a10`. It removed schema validation errors and re-proved the core server-only path, including four persisted lifecycle events, but exposed the false-success fallback defect for unavailable inventory/location and a flat `listLocations` DTO. Those failures are retained below as historical evidence rather than folded into the turn-4 result.

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

Build and deploy the turn-5 artifact server-side only. Require: invalid `giveItem` inputs return one immediate actionable `payload.error`; unavailable location rejects without mutating position; inventory times out without persisting `[]`; a ready and freshly disconnected player returns only a real observed position; the raw `listLocations` action remains nested while the standard Takaro route is reported separately; persisted lifecycle still works; visible messaging/item/teleport remain intact; the client plugin stays absent; and unsupported chat/death/entity events remain absent. Destructive actions remain approval-gated.
