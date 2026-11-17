# Empyrion position updates (issue #932)

The protocol v1 manifest declares `events.player-positions` with `intervalSeconds: 3` alongside `players.location`. The connector reads `Request_Player_List` and fresh `Request_Player_Info` records, using `PlayerInfo.pos` and the playfield dimension. It sends one complete snapshot only when all reads succeed. A snapshot exceeding 1000 entries is rejected whole, so later batches cannot contain coordinates from an older sample.

One transient position batch may be pending. The next fresh read begins at least three seconds after its exact receipt acknowledgement; a missing acknowledgement reconnects after 30 seconds. Each asynchronous read reserves a connection token, so a read crossing a reconnect or publisher shutdown is discarded. Position batches use a separate connection-scoped stream and never enter durable gameplay replay. Gameplay acknowledgements and replay IDs remain independent. Protocol v0 does not advertise or publish positions.

Chat events refresh the sender's `PlayerInfo` before including a position. If that read fails, chat is still emitted when the sender's identity is known, without attaching a stale position.

Fake-Takaro contracts cover delayed, unrelated and future acknowledgements, rejection spacing, asynchronous reads crossing reconnect or shutdown, missing-acknowledgement recovery, stable gameplay replay and whole-snapshot size rejection. Live Takaro proof for this candidate remains pending.
