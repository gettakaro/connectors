# Rust connector implementation status

The connector offers Generic Game Protocol v1 with v0 fallback. Its v1 manifest declares
`events.player-positions` with `intervalSeconds: 3`. While identified on v1, the game thread
samples online players and pushes a batch at least every 3 seconds; a chat event carries its
sender's current position. Position batches use a replaceable connection-scoped stream, separate
from durable gameplay events. No position batch is queued while disconnected or replayed after reconnect.

The protocol harness checks the manifest, event shape, distinct player IDs, and 1000-entry
limit. Live verification of Takaro's player-sync polling and `/settp` freshness is pending.
