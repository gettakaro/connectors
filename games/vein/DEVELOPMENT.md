# VEIN connector development

The Linux dedicated server loads one C++17 `libtakaro-vein.so` connector. See [INSTALL.md](INSTALL.md) for operator steps and [mod/docs/API.md](mod/docs/API.md) for the optional diagnostic HTTP interface. The former TypeScript sidecar remains in source control only as a compatibility and legacy-state migration reference; it is not built, packaged or run by the native release.

## Ownership and threads

Native transport owns TLS, WebSocket identify, heartbeat and reconnect. All libwebsockets socket calls stay on its service thread; other workers wake it through the library's safe service-wakeup mechanism. The bridge worker owns Takaro protocol messages, event formatting, log grammar, online reconciliation, durable outbox and state writes. The action worker maps the 17 Takaro actions to the existing game action functions. Threads pass **owned** messages through bounded queues. A network callback never waits for game work.

The VEIN game thread owns every UObject and engine call. Hooks enqueue minimal owned data; the game-thread pump drains within a 500 µs target and takes lazy snapshots only when an action needs them. Networking, JSON parsing, filesystem work and PCRE2 matching belong on background workers. A queued game job retains its arguments and completion state after a caller times out. Cancel jobs that have not started; let started jobs finish safely and discard late responses.

Keep the proven hook and action mechanisms: resolve symbols with the strategy chain, discover UPROPERTY offsets at runtime, and bind vtable slots on **live** objects. A resolved symbol does not prove a hook fires. `/health` reports `hooked` and `fired` counters and marks a broken capability `degraded` while keeping the game alive.

The transport requires verified certificate chain and hostname, SNI, system CAs or an explicit `TAKARO_CA_FILE`; there is no insecure fallback. Incoming WebSocket messages are capped at 1 MiB and JSON nesting at 64. Pending actions are capped at 128/4 MiB; one response at 8 MiB and queued responses at 32 MiB. Reserve connection control and response capacity, and reject overloaded requests explicitly. The event outbox holds at most 5,000 events/32 MiB, dropping the oldest on overflow with a loss counter. Events leave the durable outbox only after a **later** ping gets its pong on that same connection. Takaro supplies no per-event application acknowledgment, so this is at-least-once transmission with a possible duplicate after a lost pong, not exactly-once storage.

Persistent files are versioned and written with temporary file, fsync and rename on a background worker. Preserve `event-cursor.json`, `online-players.json`, `timed-bans.json` and `known-players.json`; game-enforcement `bans.json` is separate. Explicit file paths take precedence over `TAKARO_STATE_DIR`, which defaults to `connector-state` under `TAKARO_PLUGIN_DATA_DIR`. Corrupt timed-ban input is an error, not an empty list. Persistence failures degrade connector health without stopping VEIN. Default log grammar and named captures use PCRE2; test custom `VEIN_LOG_*_RE` values against legacy fixtures before cutover.

## Build, test and deploy

```bash
./mod/build.sh --tests          # pinned static deps, Debian Bookworm toolchain, native tests
./scripts/build-release.sh      # one takaro-vein-plugin.tar.gz plus SHA256SUMS
python3 -m unittest discover -s scripts -p test_smoke_server.py
```

The build pins libwebsockets, OpenSSL, nlohmann/json and PCRE2 and hides bundled symbols. Check the actual server image loader and `ldd` before deployment, then verify the candidate hash in the game PID's `/proc/1/maps`. A local unit pass or HTTP response cannot replace a real PC-client trigger paired with Takaro MCP event UUIDs and action responses. The [native smoke](scripts/smoke-server.py) checks exact Steam server build, game status, native connection, queue/loss/persistence fields and durable outbox; it requires an authenticated diagnostic token, but the direct Takaro connection does not.

The isolated `dev-servers/` rig has its own world and a version-locked Steam installation. One operator owns its deployment and the PC client under the rig/client locks. `dev-servers/scripts/deploy-connector.sh vein` builds the library, checks loader symbols and swaps it under the rig lock. It refuses to deploy while a legacy sidecar container still exists. Do not auto-update the server behind the test client's VEIN version; matching game versions matter, while Steam build IDs may differ.

The preload applies to `VeinServer-Linux-Test` only; SteamCMD is 32-bit. Mount the plugin directory read-only **outside** Steam's installation tree, because `app_update ... validate` removes unknown files. Never call `NetMulticast_SendChat` with a null sender, `FName::ToString` on an unvalidated value, or a game object from a background worker. A byte signature matching more than once is not a resolved symbol. Mutating Takaro actions may contain explicit JSON `null` for optional fields and a nested `player` object; preserve that behavior.

## Compatibility and acceptance

The former sidecar's action and event fixtures are the behavior reference, including offline player responses, inventory aggregation, sender names, timed bans and log filtering. The native connector must re-prove all 17 action rows and six event rows, installation/upgrades/rollback, three clean starts, outage replay, server crashes and performance with the real client and Takaro MCP. Save UTC triggers, raw MCP responses, event UUIDs, screenshots, exact game/client builds and candidate hashes in the private evidence repository before the next cell. Preserve the honest limits: log events are wire-only, entity/location coverage is incomplete and whisper isolation requires another client. Discord-to-game proof needs a human-authored post.
