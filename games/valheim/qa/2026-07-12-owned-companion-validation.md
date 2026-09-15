# Valheim Owned Companion Validation — 2026-07-12

## Outcome

The Takaro-owned graphical-client companion is live-proven with the dedicated-server connector and Takaro. The validated client-reported paths are inventory, ordinary chat and configured commands, player death, and player-attributed entity death. These observations remain untrusted gameplay claims and are not authoritative moderation, identity, anti-cheat, security, or economy evidence.

## Pinned Runtime

- Branch: `feat/valheim-owned-companion`.
- Server gameServerId: `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab`.
- Player: `Hehe`, gameId `Steam_76561198000735875`, Takaro player id `0515d777-8dc2-48b1-b035-b40dacd762c5`.
- Client companion DLL SHA-256: `d51c556d9cffa114bc04f0fa68aebf4a091d974aeddb6194ef77f5d9247704c6`.
- Client protocol DLL SHA-256: `9d116aa85a41b342b13e4bbf9437e0fc368786d54895736640ee3629ca3319d2`.
- Final authoritative-player-fix server DLL SHA-256: `f53fdec52696f65c4f166ef1ad1f4423529c939f6530909e25f6012983246a7c`.
- Product version: `2.0.0-rc.1+verify`; negotiated protocol: `1`.

The server connector was the only Takaro/cloud component on the dedicated server. The companion contained no Takaro credentials or direct cloud transport. A temporary QA-only server fixture spawned one disposable Greyling for the combat check; it was removed before the final clean server restart and was never part of either release artifact.

## Required-Mode Enforcement

- A client without the companion received `MissingCompanion`, the built-in `Kicked` RPC, and an exact-peer close.
- A test-only companion advertising protocol `2` against the server's protocol `1` received `IncompatibleProtocol`, including expected `1` and actual `2`, followed by the same bounded disconnect flow.
- The exact compatible companion negotiated protocol `1`, refreshed its five-second heartbeat, stayed connected beyond heartbeat expiry, and renegotiated on reconnect and server restart without stale-session reuse.
- Live testing exposed a real slow-load race: the original ten-second handshake deadline elapsed while the graphical client was still generating and unpacking its world. A regression test now pins the production handshake window at 30 seconds. The reproduced 17-second load then negotiated successfully, returned inventory, and was not kicked.
- A final live reconnect proved that companion reports resolve through the authoritative Valheim player list: Takaro kept `Steam_76561198000735875` online while the historical socket-derived `-1091506454` record remained offline. The same session negotiated protocol 1 and returned `getPlayerInventory` successfully.

## Takaro Action and Event Proof

- Ordinary chat `owned-companion-live-2038` persisted exactly once as `chat-message` event `c29a5f7f-eda8-4695-a4b9-c54555f92ada`.
- `$tplist` persisted exactly once as player chat event `3a2c360d-0f12-442a-afec-53ec40e39c5f` and executed as command event `27ee1262-68c2-46b3-b21b-5500702d8a24` with `success: true`. Its two expected whisper responses were separate server-originated chat events.
- Takaro repeatedly completed `getPlayerInventory` successfully. A real Takaro `giveItem` request dropped one Wood, the graphical client collected it, and one `player-inventory-changed` event recorded `Wood` from 13 to 14.
- Valheim's built-in `/die` produced exactly one `player-death` with the bound player, real position, timestamp, and message.
- A real unarmed player attack killed the disposable Greyling and persisted exactly one `entity-killed` event: `f846a5b2-34af-4621-a814-228dffdeac9e`, player `Hehe`, entity `Greyling`, weapon `Unarmed`, created at `2026-07-12T20:42:16.979Z`.

## Automated Verification

- The regression test was observed failing before the production handshake default changed and passing afterward.
- Full release test run after the fix: 404 passed, 0 failed, with warnings treated as errors.
- The real `net472` dedicated-server build against the installed Valheim/BepInEx references completed with 0 warnings and 0 errors.
- Earlier exact-branch verification also passed deterministic two-run packaging, Bash syntax, ShellCheck, formatting, setup harness, graphical-client `net472` compilation, role isolation, manifest checks, and security scans.

## Remaining Unsupported Boundary

This validation does not upgrade map actions, the upstream-blocked standard Takaro `listLocations` route, or approval-gated destructive moderation/shutdown actions. Missing, disabled, or expired companions still cannot fabricate inventory. Client-reported observations remain unsuitable as authoritative evidence.
