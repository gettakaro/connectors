# Experimental direct Takaro gate

`TAKARO_NATIVE_GATE=1` starts the direct native connection after the game
plugin's action and event hooks initialize. This opt-in developer mode is the
minimal transport test retained from the migration; normal installations use
the complete native bridge described in `../../INSTALL.md`. Stop any legacy
sidecar before enabling either native mode so the same Takaro identity never
has two active connections.

The gate reads `TAKARO_WS_URL` (default `wss://connect.takaro.io/`),
`TAKARO_IDENTITY_TOKEN` (default `vein`), `TAKARO_REGISTRATION_TOKEN`, and
`TAKARO_SERVER_NAME`. TLS verifies the certificate chain and host name against
the Debian system CA bundle. `TAKARO_CA_FILE` replaces that bundle for a
private CA; an invalid certificate always fails the connection.

The gate identifies, answers `testReachability`, `getPlayers` and
`getPlayerLocation`, and forwards the six plugin event types. Location lookup
retains the legacy 60-second join/leave fallback needed by event ingestion.
Its socket staging queue is limited to 1 MiB; an
individual event larger than 1 MiB is dropped and counted as a delivery loss.
Its outbox is held in memory, and the `native` health field
reports `gate.durableOutbox=false`. A crash can lose unconfirmed events. The
legacy sidecar cursor files are never changed by this gate. The gate must not
be treated as a production migration or used with an active sidecar.

The `native` field in authenticated `/health` reports connection state,
identification, connection epoch,
queue counts, event loss, overloads, request counts, protocol errors and the confirmed event sequence. The
confirmed sequence advances only when a pong echoes the ID of a ping sent
after those events on the same connection.
