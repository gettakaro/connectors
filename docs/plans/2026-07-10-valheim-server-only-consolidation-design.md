# Valheim Server-Only Consolidation Design

## Objective

Replace the overlapping Valheim follow-up pull requests with one reviewable change built from current `main`. The resulting connector must run only inside the Valheim dedicated server, represent server-owned capabilities honestly, package reliably in CI, and leave client-owned features explicitly unsupported.

## Constraints

- The connector must not require or activate a Takaro plugin on player clients.
- Client snapshots, client command forwarding, and custom client action RPCs are out of scope.
- Stable Steam/platform identity must come from the dedicated-server runtime.
- Empty inventory and unavailable position data must not masquerade as confirmed player state.
- Destructive live checks remain approval-gated; non-destructive server, MCP, module, and log proof is the default validation path.
- Registration and identity tokens must remain outside git and command output.

## Architecture

`TakaroValheim.dll` loads under BepInEx in the Valheim dedicated-server process. `ValheimTakaroPlugin` rejects non-dedicated processes, `TakaroWebSocketRunner` owns the outbound Generic Connector Protocol connection, `ValheimServerAdapter` maps server-owned actions, and `ValheimChatEventBridge` emits only events observable at the dedicated-server boundary.

Server-owned implementations retained from the validated local work are:

- peer/platform identity and presence;
- server-known reference position when available;
- server-side world item drops for `giveItem`;
- Valheim's built-in routed teleport RPC for `teleportPlayer`;
- routed outbound server messages;
- item, entity, and location catalogs;
- allowlisted console commands, moderation, bans, and shutdown;
- server-observable player lifecycle events.

Remote inventory and ordinary player-originated chat remain unavailable from a vanilla client at the proven server boundary. Player-death and entity-killed are also unsupported because no trustworthy server-only emitter exists. The connector emits none of those events. It returns only real current or fresh server-observed player positions; current Takaro rejects a schema-valid `payload.error` when position is unavailable. Inventory emits no response frame because its required array DTO cannot carry that error and `[]` would fabricate state.

## Consolidation Strategy

Start from current `origin/main` and implement the desired behavior directly instead of merging the divergent histories of PRs #72, #78, and #79.

- Carry forward PR #72's dedicated-server guard, structured player-state errors, and resilient SteamCMD/reference setup.
- Carry forward PR #78's malformed-chat guards and safe routed-RPC diagnostics where they remain useful without claiming chat support.
- Carry forward only PR #79's server-owned world-drop, base-game teleport, and Jotunn-removal changes.
- Exclude PR #79's optional client command bridge and every client snapshot/custom action RPC.

After the replacement PR is green, the superseded implementation/debug PRs can be closed with a pointer to the consolidated PR. Issue #69 remains open as the honest tracker for server-only inbound chat.

## Capability Contract

Every Generic Connector action and event must be documented in the Valheim README with one of these outcomes:

- `live-supported`: implemented through a server-owned path with retained live evidence;
- `schema-fallback`: reserved for an accepted result shape that is explicitly not proven game state;
- `unsupported`: fails or does not emit because no server-only path is proven.

Automated tests must prevent the reintroduction of client plugin activation, client RPC names, fake origin success, and fake empty-inventory success.

## Packaging And CI

The environment setup script will use bounded retries for transient SteamCMD/Thunderstore failures, verify downloaded reference files, and use the proven Windows-depot reference fallback when the Linux dedicated-server depot reports `Missing configuration`. Release packaging must contain the real `net472` plugin and required runtime dependencies while excluding tests and client-only artifacts.

## Verification

Verification proceeds from cheapest to strongest evidence:

1. Red-green tests for server-only guards, exact Takaro consumer failure behavior, player-position freshness, server-owned give/teleport paths, unsupported event suppression, and setup-script behavior.
2. Full Valheim solution tests, shell syntax/static checks, real `net472` build, and release package inspection.
3. Branch-wide independent verification.
4. Non-destructive live dedicated-server proof with no Takaro client plugin: startup/identity, reachability, players, location, outbound messaging, give-item world drop, teleport, catalogs, `listLocations`, and server-observable events.
5. Installed-module checks for current module inventory, safe hooks/commands where supported, and `serverMessages` cron delivery, backed by Takaro and server logs.
6. GitHub Actions must be green before the PR is considered ready.

If live infrastructure is unavailable, the PR must say exactly which proof is historical or blocked; build/unit success alone does not upgrade a capability to live-supported.

## Completion

The implementation is complete when one conventional-title PR contains the consolidated server-only changes, the branch verification and CI checks pass, no Takaro client plugin path remains, capability documentation matches behavior, and the release setup failure is addressed. Merging the replacement PR and refreshing the Valheim release PR are follow-up GitHub integration steps subject to repository review/branch protection.
