# Game-thread policy

> **The rule (Hendrik, 2026-09-17):** *avoid the game thread if we can.*

A dedicated server has exactly one game thread and it is the server's frame budget. At the
Dragonwilds rig's ~30 Hz that frame is ~33 ms, and every microsecond this plugin spends there is a
microsecond the server does not spend simulating the world. The game thread is treated as a scarce
resource: the plugin uses it only where UE gives it no choice, and it measures what it uses.

## What may run on the game thread

1. **Event capture inside a hook we installed.** The `ProcessEvent` detour and the
   `PostLogin`/`OnNetCleanup`/`Destroyed` detours are already *on* the engine's own call path. Their
   bodies are held to a few pointer reads, raw `FName` comparisons and copying owned values into the
   event ring. No `FName::ToString` in the filter, no JSON, no HTTP, no file I/O. Events are stored
   as closures over owned copies and rendered to JSON later, by the native bridge's background
   reader (`PluginState::EmitEventDeferred`).
2. **Small state copies, on demand.** Live player state (identity, ping, pawn, position, inventory)
   can only be read on the game thread. A job reads it only when a Takaro request needs it, and
   returns plain data; the calling worker builds the response.
3. **Queued mutations, under a per-tick budget.** Teleport, give, kick, ban, chat must call engine
   code. Jobs are drained from the engine `Tick` detour under `TAKARO_TICK_BUDGET_US` (default
   500 µs). Leftovers wait for the next tick; one job always runs so a slow job cannot starve the
   queue; at most 16 per tick.

## What must NOT run on the game thread

The Takaro WebSocket (TLS, framing, heartbeats) · JSON parsing and building · the durable event
outbox and every state file (fsync) · log tailing, PCRE2 grammar, redaction and noise filtering ·
plugin.log writes (`PluginLog` only enqueues; housekeeping flushes) · timed-ban scheduling ·
online reconciliation · catalogue rendering · `/proc` reads on a hot path.

## Native worker ownership

| Worker | Owns |
|---|---|
| transport (libwebsockets service thread) | the socket, TLS, ping/pong heartbeat, bounded outbound queues |
| bridge | protocol frames, request bookkeeping and the 8.5 s answer deadline, the event outbox, ring polling, raw log lines, timed-ban expiry, ban-intent recovery, reconcile scheduling |
| action | one Takaro action at a time, calling the background-callable `Actions::*` handlers |
| housekeeping (plugin init thread) | log tail polling, rate-limited game-thread maintenance jobs, plugin.log / bans.json flushes |
| diagnostic HTTP (optional) | one thread per loopback connection |

Queued game-thread jobs own their callable and their result (`std::shared_ptr`): a timeout cancels a
job that has not started; a started job may finish after its caller gave up, and nothing it touches
lives on the caller's stack.

## Concrete limits in this plugin

| Thing | Limit | Where |
|---|---|---|
| Job queue drain | `TAKARO_TICK_BUDGET_US` µs/tick (default 500, 50..33000), ≥1 job/tick, ≤16 jobs/tick, ≤256 queued | `gamethread.cpp` |
| Housekeeping entry | only when a phase has work; nobody online: one entry per 30 s | `events.cpp` |
| Class sweep (`GetObjectsOfClass`) + live PreLogin retry | on a join/leave edge, else every 5 s with players online, 30 s without | `events.cpp` |
| Health-edge poll (death fallback), connection reaper | every 5 s, only while a player is connected | `events.cpp` |
| Pending-join resolver | only while a join is pending | `events.cpp` |
| Item catalogue | built once | `actions.cpp` |
| `ProcessEvent` filter | O(1) vtable→original table; `UFunction*` → decision cache; raw `FName` compare on a miss only | `events.cpp` |
| Memory-range checks | lock-free `/proc/self/maps` snapshot; no mutex, no re-read on the hot path | `sym.cpp` |
| Engine `Tick` fired counter | resolved once at install, relaxed atomic add | `gamethread.cpp`, `hooks.cpp` |

## How it is measured

`/health.diagnostics.perf` (and `GET /debug/perf`, `?reset=1` starts a fresh window) reports the
per-tick pump time (avg / p50 / p99 / max over the last 1000 ticks, `budgetHits`), jobs per second,
**game-thread entries per second**, the `ProcessEvent` filter's calls/s, average and maximum
nanoseconds per call and cache hits, event-handler time, and every named sweep's rate, average and
maximum (`housekeep.*`, `GET /players`, `POST /ban`, …).

A change to anything in the table above is expected to come with a before/after from that endpoint,
measured on the rig.

## A/B switches

| Setting | Effect |
|---|---|
| `TAKARO_PLUGIN_IDLE=1` (or `"idle":"1"` in `plugin.json`) | The library is mapped and writes one log line, nothing else: no symbol scan, no hook, no game-thread job, no thread besides the one that logged, no socket. Arm A of an overhead A/B (server with the `.so` preloaded but idle vs. fully active). |
| `TAKARO_NATIVE_DISABLE=1` | Hooks, events and the game-thread pump run, the Takaro connection does not (isolates the cost of the native bridge itself). |
| `TAKARO_TICK_BUDGET_US` | Per-tick job budget. |

A run without `LD_PRELOAD` at all is the baseline arm; compare server tick time (the game's own
`stat`/log output or frame pacing) across the three.
