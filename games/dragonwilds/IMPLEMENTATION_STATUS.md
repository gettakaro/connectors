# Dragonwilds connector implementation status

Protocol v1 declares `events.player-positions` with a three-second minimum
interval. Position reads run through the action worker and the game's main-thread
dispatcher, so a delayed game-thread read does not block WebSocket control frames,
acknowledgments, reconnect handling, or request deadlines.

Only one snapshot read and one transient position batch can be outstanding. The
next fresh read waits at least three seconds after the matching receipt. A missing
receipt reconnects after thirty seconds. Completed reads from a previous connection
or more than three seconds ago are discarded. Position snapshots use their own
connection-scoped stream and never enter the durable gameplay outbox.

Chat events include the sender's live position when the hook can safely read it.
The existing `players.location` action remains available for command-time reads.

The production bridge harness covers delayed and incorrect receipts, reconnect
stream freshness, durable-event independence, and a blocked position read while
control traffic and reconnect handling continue.
