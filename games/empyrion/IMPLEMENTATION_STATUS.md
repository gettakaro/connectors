# Empyrion position updates (issue #932)

The protocol v1 manifest declares `events.player-positions` with `intervalSeconds: 5` alongside `players.location`. The connector reads `Request_Player_List` and a fresh `Request_Player_Info` for each online entity, using `PlayerInfo.pos` and the playfield dimension. It emits a snapshot only when all reads succeed, split into batches of at most 1000 distinct game IDs. Batches are at least five seconds apart because the publisher waits between chunks and starts its next read after a five second delay. Position snapshots use a replaceable, connection-scoped stream; they do not enter the durable gameplay outbox or replay after reconnect. Protocol v0 does not advertise or publish this event.

Chat events now refresh the sender's `PlayerInfo` before including a position. If that read fails, chat is still emitted when the sender's identity is known, without attaching a stale position.

Contract tests cover the capability parameters, fresh coordinates and event shape. Live Takaro proof remains pending because the available MCP connection returns 401. The behavior of Takaro's polling stop and `/settp` must be checked with a real game client and Takaro session before release.
