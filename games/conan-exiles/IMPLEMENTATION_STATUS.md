# Conan Exiles connector implementation status

The native connector implements protocol v1 `events.player-positions` with a declared
3-second minimum interval. Its worker samples every 3 seconds and emits a batch only
when every online player's location can be read. Position batches use a separate,
connection-scoped stream and never enter the durable gameplay outbox. Only one batch
can be in flight; the next waits until three seconds after Takaro acknowledges receipt.
Unsent snapshots are replaced by fresh samples, expire after three seconds, and are
discarded on reconnect. Missing position acknowledgements trigger a reconnect after
30 seconds.

The `chat-message` event includes the sender's position when the chat hook can read
the player's pawn. The existing `getPlayerLocation` action remains available.

Build and host tests cover these paths. Live Takaro verification of the push interval,
poll suppression, and command location is pending.
