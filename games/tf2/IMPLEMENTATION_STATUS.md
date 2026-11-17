# Implementation status

Protocol version 1 declares `events.player-positions` with a three-second minimum interval. Each batch contains all authenticated human clients in game, with their current server entity origin and map dimension, including dead players and spectators. A missing entity defers the complete batch. Chat events include the current position.

Only one position batch may await receipt. The next fresh capture waits three seconds after the exact matching receipt; unrelated, duplicate and future receipts do not release the gate. A missing receipt closes the connection after thirty seconds. Re-identification creates a new transient position stream and discards old snapshots. Gameplay event replay remains independent. Legacy protocol connections do not push positions.

The pinned release build and static contracts pass. The SourcePawn test plugin passed all36 reconnect and receipt timing assertions inside a real SourceMod server. Real-player position, chat-command and outage acceptance remains pending; successful compilation and an empty-roster connection do not qualify those checks.
