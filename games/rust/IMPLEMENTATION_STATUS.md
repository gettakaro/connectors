# Rust connector implementation status

The connector offers Generic Game Protocol v1 with v0 fallback. Its v1 manifest declares
`events.player-positions` with `intervalSeconds: 3`. While identified on v1, the game thread
samples online players and pushes a batch after the previous receipt acknowledgement plus three seconds; a chat event carries its
sender's current position. Position batches use a replaceable connection-scoped stream, separate
from durable gameplay events. No position batch is queued while disconnected or replayed after reconnect.
Only one position batch waits for acknowledgement at a time, so a slow socket cannot compress
multiple batches into a burst. Missing position acknowledgements force reconnect after 30 seconds.
The connection watchdog can retire a socket operation even when Mono leaves it pending after
cancellation or Abort, so a connection attempt made during an outage cannot prevent later retries.

The protocol harness checks the manifest, event shape, distinct player IDs, 1000-entry limit,
receipt pacing, and socket operations that ignore cancellation. On Carbon `26498655`, real
player movement updated Takaro without background location polling, and a chat `@settp`
stored the player's current position. After a connection outage, a new position stream
resumed with coordinates sampled after movement during the outage.
