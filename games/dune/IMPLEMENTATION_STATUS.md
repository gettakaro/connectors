# Dune: Awakening implementation status

## Position pushes (protocol v1)

- With `DUNE_PLUGIN_URL` configured and the plugin healthy, the v1 manifest declares `events.player-positions` at a 5-second interval. The sidecar sends a complete snapshot of the online roster at most once per interval, with unique FLS `gameId` values and at most 1000 entries. A capability replacement disables pushes when the live source fails and restores them when it recovers.
- Each pushed coordinate comes from the native plugin's live `pawn` transform. Plugin refs are joined to FLS IDs through Postgres. A missing join, missing pawn, non-finite coordinate, or plugin failure suppresses that snapshot. The sidecar never pushes the last saved Postgres transform or an old chat hint.
- The position source runs only while Takaro has an identified protocol v1 socket. Snapshots are ephemeral and are not replayed after an outage.
- Inbound game chat carries `m_OriginLocation` from the message itself as `chat-message.position` when present. The zero origin is treated as absent by the chat decoder.
- Without the optional native plugin, the sidecar cannot sample all online players' current positions. Postgres exposes their last **saved** transforms and RabbitMQ exposes a live location only when each player sends chat. The capability is therefore omitted on plugin-free deployments; Takaro continues polling `getPlayerLocation` there.

Conformance coverage: `sidecar/src/__tests__/protocol-v1.test.ts` checks the conditional manifest and interval; `position-pusher.test.ts` checks the batch shape, deduplication, interval, protocol gate, incomplete/oversized snapshots, and event field whitelist. Live Takaro acceptance remains to be checked on a server with the native plugin installed.
