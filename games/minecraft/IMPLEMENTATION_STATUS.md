# Minecraft connector implementation status

## Pushed player positions

Issue [#932](https://github.com/gettakaro/connectors/issues/932) is implemented in the
Paper, NeoForge and Fabric source. Each loader captures all online players on its server
thread every 80 ticks, with a monotonic four-second guard for catch-up ticks. The shared core
declares `events.player-positions` with `intervalSeconds: 4` on protocol v1 and sends
`player-positions` batches containing the player UUID and current position. The same
current position is attached to player chat events. Paper's asynchronous chat callback
marshals its location read to the server thread.

The protocol limit is 1000 players per batch. If more than 1000 are online, the
connector warns and skips the snapshot rather than sending a rejected payload.
Position snapshots are not generated while disconnected; a reconnect starts with a
fresh sample. Protocol v0 keeps its existing polled location behavior.

**Verification:** Core protocol tests check the capability parameters, v1 payload,
disconnect behavior, four-second guard and chat position. Core tests and Java
compilation of all five maintained targets passed in a JDK 25 container. Live
Takaro and real-client checks are pending; the feature is not yet qualified
as live-supported.
