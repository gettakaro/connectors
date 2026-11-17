# Minecraft connector implementation status

## Pushed player positions

Issue [#932](https://github.com/gettakaro/connectors/issues/932) is implemented in the
Paper, NeoForge and Fabric source. Each loader captures all online players on its server
thread, checking twice a second. A monotonic three-second guard prevents catch-up bursts. The shared core
declares `events.player-positions` with `intervalSeconds: 3` on protocol v1 and sends
`player-positions` batches containing the player UUID and current position. The same
current position is attached to player chat events. Paper's asynchronous chat callback
marshals its location read to the server thread.

The protocol limit is 1000 players per batch. If more than 1000 are online, the
connector warns and skips the snapshot rather than sending a rejected payload.
Position snapshots are not generated while disconnected; a reconnect starts with a
fresh sample. One snapshot can be in flight; the next capture waits three seconds after its matching receipt ACK. A missing receipt for 30 seconds reconnects the socket. Captures from the previous connection or older than the interval are rejected. Protocol v0 keeps its existing polled location behavior.

**Verification:** Core protocol tests check the capability parameters, v1 payload,
disconnect behavior, receipt-based pacing and chat position. Core tests and Java
compilation of all five maintained targets passed in a JDK 25 container. Live
Takaro and real-client checks are pending; the feature is not yet qualified
as live-supported.
