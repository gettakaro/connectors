# SCUM connector implementation status

## Player positions (issue #932)

The protocol v1 manifest declares `events.player-positions` with a three-second minimum interval. The connector reads the current location of each online prisoner's actor on the UE4SS game thread and keeps at most one transient position batch in flight. The next interval starts only after its matching acknowledgement; a missing acknowledgement reconnects after 30 seconds. Each entry uses the player's SteamID64 as `gameId`, SCUM world coordinates in centimeters, and map dimension `the_island`. An absent prisoner is omitted; the last-known location retained for the legacy `getPlayerLocation` action is never used in a batch. Snapshots are sent only on an identified v1 connection and kept separate from durable gameplay event acknowledgement and discarded on disconnect, so reconnects cannot replay old positions. The chat RPC includes a position when its player controller has a current prisoner; the chat-log fallback can omit it.

Protocol tests cover the manifest, delayed and missing acknowledgements, rejection spacing, stable gameplay replay IDs, fresh reads after transport failures or epoch changes, and live-actor filtering. Native compilation and the UE4SS import check are part of the pinned target package build.
