# Position push status

Issue [gettakaro/connectors#932](https://github.com/gettakaro/connectors/issues/932)

- Protocol v1 declares `events.player-positions` with `intervalSeconds: 5`.
- The dedicated server captures all online players' current peer or public positions on the Unity thread. It publishes a batch only when every player has a real observation, at most once per five seconds. Missing observations postpone a batch; cached locations are not substituted.
- A peer-bound `chat-message` includes the sender's current server-observed peer position when available.
- Reference-free tests and a real-assembly plugin build cover the wire shape and compilation. A live game-client check through Takaro is still required before marking the event `live-supported`.
