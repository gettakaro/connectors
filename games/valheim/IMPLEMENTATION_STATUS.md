# Position push status

Issue [gettakaro/connectors#932](https://github.com/gettakaro/connectors/issues/932)

- Protocol v1 declares `events.player-positions` with `intervalSeconds: 3`.
- The dedicated server captures all online players' current peer or public positions on the Unity thread. It publishes a batch only when every player has a real observation, only after the previous batch is acknowledged and at least three seconds have elapsed. Missing observations postpone a batch; cached locations are not substituted. Position batches use a connection-scoped stream and are discarded on disconnect rather than entering the replay queue.
- A matching position acknowledgement starts the minimum interval; one batch is in flight at a time, so stalled writes cannot compress receipt times. A missing acknowledgement reconnects after 30 seconds.
- A peer-bound `chat-message` includes the sender's current server-observed peer position when available.
- Reference-free tests and a real-assembly plugin build cover the wire shape and compilation. A live game-client check through Takaro is still required before marking the event `live-supported`.
