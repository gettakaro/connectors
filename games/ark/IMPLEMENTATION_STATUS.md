# ARK: Survival Evolved implementation status

## Player positions (issue #932)

The Linux 21241282 native plugin reads each online player's current pawn location
on the game thread every 250 ms. A successful read is copied into the gate's
bounded position snapshot. The protocol v1 manifest declares
`events.player-positions` with `intervalSeconds: 5`. The WebSocket thread sends
one batch per interval, including only online players whose pawn sample is less
than two seconds old. Each row has a stable Steam64 `gameId`, `x`, `y`, `z`, and
`dimension: TheIsland` only when that calibrated map is active. Empty batches
are sent when no live position is available. This avoids replaying prior
positions after a disconnect or reconnect.

The global `chat-message` event includes `position` when the game-thread pawn
sample is fresh at chat capture time. It omits the field if the pawn is absent
or the sample is stale. `getPlayerLocation` remains supported for protocol v0
and callers that explicitly request it; its retained death and departure
positions are excluded from push batches.

Source and fake Takaro contract tests are implemented. Real client movement,
Takaro `eventSearch`, and tracking readback remain unverified for this change.
