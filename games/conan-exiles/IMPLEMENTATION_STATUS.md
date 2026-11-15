# Conan Exiles connector implementation status

The native connector implements protocol v1 `events.player-positions` with a declared
4-second minimum interval. Its worker samples every 5 seconds to allow for delivery
jitter. It reads one online-player snapshot and emits a batch only when all locations
can be read. Position batches are sent only on an identified v1 connection. Older
unconfirmed batches are discarded when replaced or after a disconnect, so stale
locations are not replayed.

The `chat-message` event includes the sender's position when the chat hook can read
the player's pawn. The existing `getPlayerLocation` action remains available.

Build and host tests cover these paths. Live Takaro verification of the push interval,
poll suppression, and command location is pending.
