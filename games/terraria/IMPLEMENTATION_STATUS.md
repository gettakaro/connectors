# Terraria connector implementation status

## Player positions (issue #932)

- Protocol v1 declares `events.player-positions` with `intervalSeconds: 3`.
- The TShock plugin's `/takaroposall` command captures all active player positions
  in one REST command response. Coordinates are world pixels, matching `getPlayerLocation`.
- The bridge reads a fresh snapshot at least 3 seconds after the previous matching receipt acknowledgment. Only one position batch can be in flight; a missing acknowledgment reconnects after 30 seconds. It
  validates the complete batch, rejects duplicate player IDs and oversized
  batches, and stops polling on disconnect. Position snapshots bypass the durable
  v1 event buffer and use a new stream on each connection, so stale positions cannot replay.
- The bridge can forward a chat event's `position` when present. Current TShock
  text-log chat events do not carry a position at the instant of chat.
- The plugin build, bridge build and bridge tests pass. Live Takaro polling
  suppression and `/settp` freshness remain unverified.
