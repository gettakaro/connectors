# Valheim Server-Only Validation Ledger

## Verdict: FAIL (turn-9 exerciser and exact Codex review passed; two severity-8 findings require turn 10)

Turn 9 verified exact source commit `0a008863d4761a7865c97efdc88bd598204a978d`. Its exact prerelease artifact loaded, identified, and passed the safe live exerciser with an unmodified vanilla client. Locale-stable release validation passed and exact Codex review session `019f5004-5dcb-7751-9693-fea212ea19a8` completed. The branch verdict remains `FAIL` because that review found two severity-8 defects: game adapter calls could run on the WebSocket background thread, and Windows compile-reference fallback could replace a configured Linux server installation. Turn-10 source changes do not inherit turn-9 verification. Fresh branch verification, PR creation, GitHub Actions, merge, and release refresh remain pending orchestrator work.

Destructive kick, ban, unban, and shutdown actions remained deliberately skipped and approval-gated.

## Turn-9 Exact Artifact and Verification Findings

| Evidence | Exact value | Result |
| --- | --- | --- |
| Source commit | `0a008863d4761a7865c97efdc88bd598204a978d` | Exact clean turn-9 HEAD |
| Prerelease version | `7.8.9-rc.2+verify9` | Exact package/informational SemVer |
| Release zip SHA-256 | `1fa0ac9eb9bc7ca0f5b2cc296eff1df7abefdb8402f20e5fbaa226c93bf99f51` | Exact verifier artifact |
| Plugin DLL SHA-256 | `5c7728ffbdd33844547f7aa5f9d921b9c698f3b2d22cf5c684a54bc8c0e7466f` | Exact verifier plugin |
| Unit/contract tests | `174/174` | PASS |
| Setup behavior harness | `26/26` | PASS at turn-9 source |
| BepInEx loader | `Takaro Valheim 7.8.9` | PASS: prerelease loader metadata accepted |
| Game server ID | `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab` | PASS: exact server identified |
| Client boundary | Vanilla client; no Takaro client DLL | PASS: dedicated-server-only |
| Real server-owned position | `140/33/-2` | PASS |
| Persisted `player-connected` | `55ab985e-d0e2-4b6b-9d0a-8c3df7d365dd` | PASS |
| Persisted `player-disconnected` | `8688cb9f-3540-4e34-b355-4a0aa52d69b8` | PASS |
| Outbound chat record | `53026329-cdd3-4d81-b6ea-d321931c47fb` | Acting-user outbound message, not connector-emitted inbound chat |
| Safe runtime exercise | `Exerciser: PASSED` | PASS: safe actions and unsupported-event exclusion |
| Exact Codex review | `019f5004-5dcb-7751-9693-fea212ea19a8` | COMPLETED; two severity-8 findings |
| Evidence directory | `/tmp/valheim-turn9-evidence` | Local sanitized evidence; secrets excluded |

The turn-9 live window proved the real position, lifecycle persistence, inventory non-mutation, actionable invalid/unsupported failures, safe outbound messaging, and zero connector-emitted chat-message, player-death, or entity-killed events. It did not exercise a valid world drop, so it did not expose the background-thread `ItemDrop` path. The run reused existing references, so it also did not cross the unsafe Windows fallback boundary. Destructive actions stayed skipped and are not upgraded by this run.

Turn 10 adds a bounded main-thread scheduler for every request-dispatched adapter action and lifecycle read, with `ValheimTakaroPlugin.Update()` as the drain boundary. It also separates writable compile references into an owned cache and refuses to mutate invalid non-empty unowned or live-server directories. These are source changes awaiting independent turn-10 verification, not retroactive turn-9 proof.

## Turn-8 Exact Artifact and Verification Failure

| Evidence | Exact value | Result |
| --- | --- | --- |
| Source commit | `92320c76db97da8972237cde72ac8bed59f880c8` | Exact clean turn-8 HEAD |
| Prerelease version | `7.8.9-rc.2+verify8` | Exact package/informational SemVer |
| Release zip SHA-256 | `4d1d7f2497546beb0051d04d4b6b17ae3bf73e5888cdccae26daf3df449c6724` | Exact verifier artifact |
| Plugin DLL SHA-256 | `c235f20e968e7b282f95639c7a60ec704335b2519a54c44d0d1f1bf5c39eea5f` | Exact verifier plugin |
| BepInEx loader | `Takaro Valheim 7.8.9` | PASS: numeric loader metadata accepted |
| Game server ID | `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab` | PASS: exact server identified |
| Client boundary | Vanilla client; no Takaro client DLL | PASS: dedicated-server-only |
| Real server-owned position | `140/33/-2` | PASS |
| Persisted `player-connected` | `1311f9bd-c636-4fdd-9793-335aa6547285` | PASS |
| Persisted `player-disconnected` | `6a9531e6-6ccf-4cb8-9edb-1c9bdf607dc5` | PASS |
| Safe runtime exercise | `Exerciser: PASSED` | PASS: safe actions and unsupported-event exclusion |
| Release validation | `1.2.3-é` accepted under `en_US.UTF-8` and packaged; rejected under `C.UTF-8` | FAIL: locale-dependent release validation |
| Independent review | `Codex review: BLOCKED by quota until 20:22` | BLOCKED, not passed |
| Evidence directory | `/tmp/valheim-turn8-evidence` | Local exact-turn evidence; secrets excluded |

No unsupported chat-message, player-death, or entity-killed event was emitted during the exact turn-8 window. The safe action suite passed without a client plugin. Destructive checks remained skipped and are not upgraded by this run.

The turn-8 locale finding is a release blocker even though the runtime exerciser passed: ambient locale changed the meaning of `[A-Za-z]` in Bash's SemVer regular expression. Turn 9 scopes matching to the C locale inside the sourced resolver and adds executable resolver and build-release tests across available `C`, `C.UTF-8`, and `en_US.UTF-8` locales. Those turn-9 changes require fresh independent verification.

## Turn-7 Failed Artifact and Numeric Control

| Evidence | Exact value | Result |
| --- | --- | --- |
| Source commit | `36730faec109f9975865492d9cc619ab12f5fc7f` | Exact clean turn-7 HEAD |
| Prerelease version | `7.8.9-rc.2+verify7` | Full SemVer was incorrectly passed to the BepInEx loader attribute |
| Failed release zip SHA-256 | `5d24cf113e1235c6b51844a5d3f4cbe2380be0e0105888aa92d191753bbfda88` | Exact verifier artifact |
| Failed plugin DLL SHA-256 | `bb74d96f6606736d66956b7cbe3746b5731c0921e38c07c4540f0022e0d6231a` | Exact packaged/deployed DLL |
| Prerelease runtime | BepInEx logged `because its version is invalid` | FAIL: plugin skipped; zero Takaro identify lines |
| Independent review | `Codex review: BLOCKED by quota until 20:22` | BLOCKED, not passed |

The numeric-version control `7.8.9` from the same source commit loaded and identified game server `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab`. A vanilla client joined without a Takaro client plugin, the server-owned location persisted as `140/33/-2`, and Takaro persisted lifecycle events `247a346b-c69d-47b1-b9c9-d28cc4a74d60` (`player-connected`) and `aae4df31-7660-4447-8103-8447eb639518` (`player-disconnected`). Safe unsupported/invalid-input and inventory non-mutation checks passed. Destructive kick, ban, unban, and shutdown actions remained skipped and approval-gated.

This control isolates the turn-7 failure to release metadata; it does not make the failed prerelease artifact releasable. Turn 8 separates numeric BepInEx metadata from full package SemVer and requires fresh independent verification.

### Turn-8 local build evidence (not independent or live proof)

Turn-8 player gates passed `172/172` tests, `24/24` setup scenarios, Bash/JSON/diff/title checks, and a real `net472` build with zero warnings and zero errors. ShellCheck was unavailable locally. Two release packages were built against real dedicated-server references:

| Version | ZIP SHA-256 | Plugin DLL SHA-256 | Core DLL SHA-256 | Metadata result |
| --- | --- | --- | --- | --- |
| `1.0.0` | `e4ae818224acd7c36e31722480e412638fd01c1f49ee9354e3178bfb4a11f8c7` | `9544033124bb5c5e53eb3f3232d29f36361cfbb30efe8cf21cd4b79796324435` | `373819238d3e871fbcab261c4b90fb6704eb9ebf4867c66c270698b3374e5408` | BepInEx `1.0.0`; informational `1.0.0`; assembly `1.0.0.0` |
| `7.8.9-rc.2+verify8` | `41d27290de50fc5075ef8e340da8a21761956eed1f0d011791e1edeaad5db56c` | `03bae4a2c2dc06447c4aff0207be2a0397d623272eeaf8617c465290784ba195` | `4766c9fa9757d74b4b45990f94d4808e178ea362b06a147c08b70978ed3f166f` | BepInEx `7.8.9`; informational `7.8.9-rc.2+verify8`; assembly `7.8.9.0` |

A non-loading `System.Reflection.Metadata` probe read the actual packaged `BepInPlugin` custom attribute and passed its loader value through `System.Version`. Both manifests and packaged READMEs retained the exact full SemVer. Package inspection found no client artifact, Jotunn dependency, test payload, or banned custom client RPC marker. The independent turn-8 verifier then live-loaded its own exact prerelease artifact as recorded above; the separate locale gate still failed.

## Turn-6 Artifact-Pinned Evidence

### Artifact, build, and boundary

| Evidence | Exact value | Result |
| --- | --- | --- |
| Source commit | `20bed2475ad558646c4c7cfccb20a185e516a429` | Exact clean turn-6 HEAD |
| Release zip SHA-256 | `d322af0b405fbc901a48f5a5f0c1b9c1f052167ab05295acdc53896395a97186` | Deployed build input |
| Packaged/deployed DLL SHA-256 | `028eb5dfda9e52eb9998d3c538c4189e6332e761ad563a23ba8b76cdecc61755` | Exact match |
| Unit/contract tests | `158/158` | PASS |
| Setup behavior harness | `19/19` | PASS |
| Real plugin build | real `net472` build: PASS with 0 warnings | PASS |
| Runtime exercise | `Exerciser: PASSED` | PASS |
| Independent review | `Codex review: BLOCKED by quota until 20:22` | BLOCKED, not passed |
| Game server ID | `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab` | Exact identified server |
| Evidence directory | `/tmp/valheim-turn6-evidence` | Local exact-turn evidence; no secrets committed |

The live window ran from `2026-07-10T16:20:47Z` until the server shutdown trace at `18:25:10` Europe/Brussels. Vanilla player `Hehe` connected with an empty client-plugin scan, and the dedicated-server DLL exactly matched the packaged DLL. Server and client processes were stopped after the safe checks; cleanup complete.

### Runtime availability and safe action evidence

- The pre-ready harness identified successfully but emitted no game events. Unavailable `getPlayers`, `listItems`, `listEntities`, `listLocations`, `listBans`, and inventory requests produced zero response frames rather than fabricated empty state. `getPlayer` returned an immediate `runtime_unavailable` payload error.
- `getMapInfo` and `getMapTile` returned immediate `server_only_unsupported` payload errors, matching their registry classification.
- Once ready, `getPlayers` returned vanilla player `Hehe`. Oversized `giveItem` amount `1001` returned an actionable HTTP 400 in approximately 387 ms.
- The inventory probe produced zero response frames, and Takaro inventory history remained empty with no fabricated inventory changes.
- The player-on-game record held the real server-observed position `85/36/-2` while online and retained `85/36/-2` after disconnect; it was never replaced with an origin placeholder.

### Persisted lifecycle and exclusions

- Persisted `player-connected`: `4e0aa0c0-d5da-4558-be9b-61c906b5bcfc`.
- Persisted `player-disconnected`: `63c912ff-5c5e-402f-8f4e-1b31ece68ce3`.
- The final exact-window search contained exactly those two lifecycle events. Unsupported chat-message, player-death, and entity-killed events remained absent.
- Destructive `kickPlayer`, `banPlayer`, `unbanPlayer`, and `shutdown` implementation exists, but exact live support remains unproven and approval-gated. The checks were deliberately skipped and these actions are classified `unsupported` until an approved disposable-server run proves the current artifact.

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

Task 7 is complete for the exact turn-6 commit and artifact above. Turn-7 local player gates passed `166/166` tests, `20/20` setup scenarios, Bash/JSON checks, and a real `net472` build with zero warnings or errors, but those checks missed loader compatibility. The exact `7.8.9-rc.2+verify7` verifier artifact was then rejected by BepInEx, so the turn-7 exerciser and overall branch verdict are `FAIL`. The separate numeric-version control evidence is recorded above and is not substituted for the failed artifact.

Turn-9 safe live verification, locale/release gates, and exact Codex review completed, but branch verification failed on the two severity-8 thread/cache findings recorded above. Turn-10 branch verification remains pending under Task 8 before PR handoff. PR creation, GitHub Actions, superseded-PR closure, merge, and release refresh remain pending orchestrator work. Destructive actions remain approval-gated and are not required for this server-only validation ledger.
