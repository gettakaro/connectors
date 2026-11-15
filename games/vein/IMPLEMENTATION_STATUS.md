# VEIN connector implementation status

## Pushed player positions

The native plugin advertises `events.player-positions` on protocol v1 with a five-second interval. Its action worker takes a fresh player snapshot for each batch and sends up to 1000 unique online, spawned players whose pawns have finite coordinates. A connected player without a possessed character cannot have a truthful position yet and is omitted until the pawn is available.

Position batches use a process-scoped ephemeral stream with increasing sequence numbers. They are not stored in the durable gameplay-event outbox, so reconnecting does not replay stale coordinates. The existing `players.location` action remains available for Takaro's live read before commands. The released sidecar remains a legacy compatibility path; this capability is in the native plugin only.

The server chat RPC identifies the sender's player state but does not reliably carry the possessed pawn. Chat lines from the log tail have no live entity either, so chat events do not claim a position from those sources.

Manifest conformance is covered by `mod/tests/native_behavior_parity.cpp`. A live Takaro gate and `/settp` check remain pending.
