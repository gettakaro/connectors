# ARK: Survival Evolved implementation status

## Player positions (issue #932)

The Linux 21241282 native plugin reads each online player's current pawn location
on the game thread every 250 ms. A successful read is copied into the gate's
bounded position snapshot. The protocol v1 manifest declares
`events.player-positions` with `intervalSeconds: 3`. The WebSocket thread sends
one outstanding batch and waits the declared interval after its matching receipt
before reading the next snapshot, including only online players whose pawn sample is less
than two seconds old. Each row has a stable Steam64 `gameId`, `x`, `y`, `z`, and
`dimension: TheIsland` only when that calibrated map is active. Empty batches
are sent when no live position is available. This avoids replaying prior
positions after a disconnect or reconnect. Every connection has a fresh position
stream; a missing receipt closes the connection after 30 seconds. Position receipts
never advance the durable gameplay event cursor.

The global `chat-message` event includes `position` when the game-thread pawn
sample is fresh at chat capture time. It omits the field if the pawn is absent
or the sample is stale. `getPlayerLocation` remains supported for protocol v0
and callers that explicitly request it; its retained death and departure
positions are excluded from push batches.

Current pawn snapshots, positioned chat, and fresh reconnect streams are supported
on Linux 21241282. Position acknowledgements pace only the transient stream;
gameplay events retain their durable delivery cursor.
