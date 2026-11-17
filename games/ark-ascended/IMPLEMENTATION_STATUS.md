# ARK: Survival Ascended position batches

Issue #932 is implemented in the AsaApi plugin for protocol v1. The capability manifest declares
`events.player-positions` with `intervalSeconds: 3`. The game-thread tick samples the live
`PlayerControllerList` and calls `GetPosition` only for online players with a spawned character.
It includes at most 1000 distinct EOS `gameId` entries per batch.
Each coordinate is finite and uses the same map dimension as `getPlayerLocation`.

The chat callback reads the sender's current position at message time. It omits `position`
when the sender has no character or the game returns invalid coordinates; it never substitutes
the roster's last position. Server chat has no player and no position.

Position reads reserve a connection-generation ticket before touching game objects. A read
crossing a reconnect, stop, or two-second freshness deadline is discarded. A separate
transient stream permits one outstanding batch; only its matching application ACK starts
the next three-second wait. No-ACK sessions reconnect after 30 seconds. Positions bypass
the durable gameplay window and expire before transport write, so delayed snapshots do
not block chat or replay after reconnection.

`getPlayerLocation` remains available for direct requests and for offline last-known position;
its behavior is separate from the live position stream.

## Verification

- `tests/client_test.cpp` covers v1 shape, interval, replacement, reconnect discard, and v0 exclusion.
- `tests/check_capabilities.py` checks the manifest and capability registry match the source.
- Windows DLL compilation and a real Takaro/client check are still required. `liveVerified` stays
  false in `capabilities.json` until that run.
