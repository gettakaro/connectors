# Enshrouded connector implementation status

## Player positions (issue #932)

The native connector declares `events.player-positions` for Generic protocol v1 with `intervalSeconds: 3`. Once identified, it reads the current online list and each player's location from the in-process plugin on an action worker. It emits a batch only when every live read succeeds, with unique `gameId` values and at most 1000 positions. Failed, slow, or connection-crossing reads are discarded. Position batches are transient events: they are never saved in the durable event outbox or replayed after reconnect. Only one position batch awaits acknowledgement; the next read starts three seconds after Takaro acknowledges the prior batch, so a stalled transport cannot compress receipt intervals. An acknowledgement timeout reconnects. Successful reads also refresh the existing disconnect-position cache.

The game plugin's chat receive hook supplies a chat handle and resolved player name, but does not supply a world-thread-safe transform for that message. Chat events therefore omit `position` until the hook can capture the sender's entity transform safely at receive time. The separate player-position batches provide current coordinates without associating a later location with an earlier message.

Host conformance covers the v1 capability, the position batch payload, acknowledgement pacing and reconnect stream reset. Live verification on game build 1287369 confirmed fresh position updates, cessation of background location polling, current coordinates saved by a real chat teleport command, and fresh snapshots after reconnect. Other supported game builds still need live verification.

Position snapshot reads have an independent five-second deadline. A queued read that expires is cancelled and releases the capture gate; an overlong running read reconnects and its late completion is discarded. Internal reads never emit request responses or increment API request timeout counters. Health reports position read timeouts separately. Missing position receipts reconnect after thirty seconds.
