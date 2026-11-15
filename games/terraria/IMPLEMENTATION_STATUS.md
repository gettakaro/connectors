# Terraria connector implementation status

## Player positions (issue #932)

- Protocol v1 declares `events.player-positions` with `intervalSeconds: 5`.
- The TShock plugin's `/takaroposall` command captures all active player positions
  in one game-thread read. Coordinates are world pixels, matching `getPlayerLocation`.
- The bridge requests one snapshot every 5 seconds after identification. It
  validates the complete batch, rejects duplicate player IDs and oversized
  batches, and stops polling on disconnect. The v1 event buffer may replay
  an unacknowledged snapshot after reconnect before the next fresh one arrives.
- The bridge can forward a chat event's `position` when present. Current TShock
  text-log chat events do not carry a position at the instant of chat.
- The plugin build, bridge build and bridge tests pass. Live Takaro polling
  suppression and `/settp` freshness remain unverified.
