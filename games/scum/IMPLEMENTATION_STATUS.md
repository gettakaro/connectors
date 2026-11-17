# SCUM connector implementation status

## Player positions (issue #932)

The protocol v1 manifest declares `events.player-positions` with a three-second minimum interval. The connector reads the current location of each online prisoner's actor on the UE4SS game thread and keeps at most one transient position batch in flight. The next interval starts only after its matching acknowledgement; a missing acknowledgement reconnects after 30 seconds. Each entry uses the player's SteamID64 as `gameId`, SCUM world coordinates in centimeters, and map dimension `the_island`. An absent prisoner is omitted; the last-known location retained for the legacy `getPlayerLocation` action is never used in a batch. Snapshots are sent only on an identified v1 connection and kept separate from durable gameplay event acknowledgement and discarded on disconnect, so reconnects cannot replay old positions. The chat RPC includes a position when its player controller has a current prisoner; the chat-log fallback can omit it.

Source tests cover the manifest, interval, payload, reconnect behavior, and live-actor filtering. End-to-end verification in Takaro is pending: live proof is still pending. The existing six-event coverage registry records historical live evidence and does not classify this new event as live-supported until that proof exists.
