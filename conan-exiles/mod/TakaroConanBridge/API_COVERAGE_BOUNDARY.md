# Takaro API Coverage Boundary

The final-goal machine-readable action/event matrix is `takaro-mod/API_GOAL_MATRIX.json`.
It separates connector proof, TakaroConan-required proof, schema fallbacks,
unsupported actions, and approval-gated destructive checks.

`capabilities.json` also carries `finalTakaroConanGoal`. That field must not be
read as connector capability support. It is the current final-goal declaration
and must point operators at `apply-ready-takaro-inputs.sh --post-reconnect-check`
until a live receipt proves the installed server/client `TakaroConan.pak`
replaced Pippi.

The final audit runs `validate-final-goal-status.mjs --require-validated`.
After the live receipt and QA ledger prove the replacement, update
`finalTakaroConanGoal.status` from `not-validated` to `validated`; until then,
the final audit must continue to fail.

When setting `status="validated"`, include
`finalTakaroConanGoal.evidence.validatedAt`, `liveReceipt`, `finalAuditLog`,
`qaLedgerSection`, and `postReconnectCommand`. The status validator checks that
the referenced live receipt exists and passes
`validate-live-receipt.sh --receipt`, including TakaroConan source attribution,
player-visible server-wide and targeted chat proof, inbound chat proof with
stable player identity, Pippi/RCON renderer absence, and a fresh player-triggered
Takaro module command event from the same validation run.

Use `mark-final-goal-validated.mjs` for that transition instead of editing the
JSON by hand. It runs the strict live receipt validator, checks that the
provided final-audit log contains passed install/reconnect, live mod, and live
receipt gates, writes the evidence object, and then reruns
`validate-final-goal-status.mjs --require-validated`.

Validate it with:

```bash
node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-api-goal-matrix.mjs
```

Installed Takaro module automation inventory is validated separately:

```bash
node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-module-automation.mjs
```

The connector-owned Takaro surface is the Generic Connector game-server action/event protocol implemented in `src/takaro/protocol.ts`.

Current connector actions:

- `getPlayer`
- `getPlayers`
- `getPlayerLocation`
- `testReachability`
- `executeConsoleCommand`
- `listBans`
- `listItems`
- `listEntities`
- `listLocations`
- `getPlayerInventory`
- `getMapInfo`
- `getMapTile`
- `giveItem`
- `sendMessage`
- `teleportPlayer`
- `kickPlayer`
- `banPlayer`
- `unbanPlayer`
- `shutdown`

Current connector events:

- `player-connected`
- `player-disconnected`
- `chat-message`
- `player-death`
- `entity-killed`
- `log`

Every item above must have one explicit status in `src/takaro/coverage.ts` and `capabilities.json`: `live-supported`, `schema-fallback`, or `unsupported`.

The Takaro Conan `.pak` is required for the chat/event boundary only:

- `sendMessage` server-wide normal chat rendering.
- `sendMessage` targeted normal chat rendering.
- `chat-message` inbound event emission with stable player identity.

Those entries must keep `goalStatus="takaro-mod-required"` and
`finalProofGate="TakaroConan live mod gate"` in `API_GOAL_MATRIX.json`. Pippi
server/directmessage output or Pippi ChatWindow logs remain historical
connector proof, not final TakaroConan proof.

The existing sidecar remains the correct owner for RCON actions, save DB reads, Takaro WebSocket connectivity, `/health`, and coverage classification.

The MCP server also exposes platform tools such as module, hook, command, cronjob, event, analytics, shop, Discord, user, role, domain, and game-server admin endpoints. Those tools can validate Takaro automation around this server, but they are not implemented inside the Conan connector or the Conan `.pak`.
