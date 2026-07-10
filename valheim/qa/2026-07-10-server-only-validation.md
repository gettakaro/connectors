# Valheim Server-Only Validation Ledger

## Verdict: PASS WITH GAPS

Source commit `20b505b2fcc5e58a6bdb0ec3bf4d26bda6a5f096` passed build/package gates and a live dedicated-server run with a vanilla player. Player actions, catalogs, visible messaging, and cron delivery were proven. Client-owned inventory remains a schema fallback; inbound chat, death, and entity-killed remain unsupported; lifecycle frames were written but exact Takaro persistence was not proven.

Turn-3 source is not live-validated by this ledger. Later commits may cite the executable tests here, but must not inherit the live verdict until their own artifact is deployed and exercised.

## Runtime Boundary

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

Deploy the turn-3 artifact server-side only, repeat unavailable inventory/location polling, reconnect/disconnect the vanilla player, and require: no Takaro DTO validation errors; exact `eventSearch` lifecycle records before promotion; continued visible messaging/item/teleport proof; client plugin absence; and no unsupported event emission. Destructive actions remain approval-gated.
