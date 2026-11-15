# Enshrouded connector implementation status

## Player positions (issue #932)

The native connector declares `events.player-positions` for Generic protocol v1 with `intervalSeconds: 5`. Once identified, it reads the current online list and each player's location from the in-process plugin on an action worker. It emits a batch only when every live read succeeds, with unique `gameId` values and at most 1000 positions. Failed, slow, or connection-crossing reads are discarded. Position batches are transient events: they are never saved in the durable event outbox or replayed after reconnect. Successful reads also refresh the existing disconnect-position cache.

The game plugin's chat receive hook supplies a chat handle and resolved player name, but does not supply a world-thread-safe transform for that message. Chat events therefore omit `position` until the hook can capture the sender's entity transform safely at receive time. The separate player-position batches provide current coordinates without associating a later location with an earlier message.

Host conformance covers the v1 capability and the position batch payload. Live Takaro acceptance, including sync and teleport checks, remains unverified.
