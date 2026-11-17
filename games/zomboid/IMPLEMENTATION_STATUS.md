# Project Zomboid implementation status

The Build 42 dedicated server exposes each connected character's coordinates through
`IsoPlayer`. The connector reads them on the game thread.

| Capability | Implementation | Validation |
|---|---|---|
| `players.location` | Reads a player's current coordinates on request. | Existing live coverage. |
| `events.player-positions` | Protocol v1 declares a 3-second receipt-paced interval and pushes one timestamped batch for all connected players with live character positions. Push capability is withdrawn while a connected player is dead or loading without readable coordinates, allowing Takaro to poll living players. Complete-roster reads retry every three seconds and restore push once every player is readable. Snapshots use a connection-specific transient stream with one in flight; they never enter the gameplay event outbox. | Protocol tests pass; live Takaro check pending. |
| `events.chat-message` position | Includes the author's live position when the chat hook can resolve their character. | Protocol tests pass; live Takaro check pending. |

Protocol 0 keeps its existing event and location behavior. See [DEVELOPMENT.md](DEVELOPMENT.md)
for the protocol details.
