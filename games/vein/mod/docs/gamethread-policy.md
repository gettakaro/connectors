# Game-thread policy

> **The rule (Tester, 2026-09-17):** *avoid the game thread if we can.*

A dedicated server has exactly one game thread and it is the server's frame budget. At the VEIN
rig's ~30 Hz that frame is ~33 ms, and every microsecond this plugin spends there is a microsecond
the server does not spend simulating the world. So the game thread is treated as a scarce,
non-renewable resource: we use it only where UE gives us no choice, and we measure what we use.

## What may run on the game thread

1. **Event capture inside a hook we installed.** A `ProcessEvent` detour or a `PostLogin`/`Logout`
   detour is already *on* the engine's own call path — there is no "off-thread" version of it. The
   detour body is therefore held to: a few pointer reads, raw `FName` comparisons, and handing the
   result on. No `FName::ToString` in the filter, no string compares per call, no JSON building, no
   HTTP, no allocation-heavy work.
2. **One small state copy, at a low rate, on demand.** Live player state (identity, ping, pawn
   class, position) can only be read from the game thread. It is copied into plain-data rows at most
   once per `TAKARO_SNAPSHOT_TTL_MS` (default 500 ms), **and only when a request actually needs it**.
   No request, no entry.
3. **Queued mutations, under a strict per-tick budget.** Teleports, gives, kicks, messages and admin
   grants must call engine code. They are queued and drained from the Tick hook under
   `TAKARO_TICK_BUDGET_US` (default 500 µs). Leftovers wait for the next tick; one job always runs so
   a slow job cannot starve the queue.

## What must NOT run on the game thread

JSON building · HTTP serving · catalogue and entity lists (built once at boot or on demand, then
served from an immutable cache) · log tailing and log parsing · name humanising · dedupe and
aggregation · redaction · admin id parsing · `/proc` reads · anything that can be answered from the
last snapshot.

**If a read can be served from the last snapshot, serve it, and do not touch the game thread.**

## Native worker ownership

The transport worker owns the socket and libwebsockets service context. Socket callbacks
only move owned messages through bounded queues; they never wait for actions or storage.
The bridge worker handles protocol messages, events, and persistent state. A separate
action worker invokes the existing game handlers. Network, JSON, filesystem, and regex
work belong on background workers, outside the engine tick.

Queued jobs must own their callable arguments and completion storage. A timeout cancels
a job that has not started. A started job may finish after its caller returns, keeping
its data alive and discarding the late result. Capturing a caller's local variables by
reference is unsafe even when an outer completion object uses shared ownership.

## Concrete limits in this plugin

| Thing | Limit | Where |
|---|---|---|
| Job queue drain | `TAKARO_TICK_BUDGET_US` µs/tick (default 500), ≥1 job/tick, ≤16 jobs/tick | `gamethread.cpp` |
| Player snapshot | lazy, TTL `TAKARO_SNAPSHOT_TTL_MS` (default 500 ms) | `actions.cpp` |
| Class sweep (`GetObjectsOfClass`) | on join/leave edge, else every 15 s; resumable, ≤1 ms per entry | `events.cpp` |
| Health-edge poll (death fallback) | every 5 s, and only while a player is connected | `events.cpp` |
| Connection reaper | every 5 s, and only while a player is connected | `events.cpp` |
| Admin grant pass | on the join edge, else every 60 s | `admin.cpp` |
| Item catalogue | built once at boot | `actions.cpp` |
| Entity catalogue | cached 5 min | `actions.cpp` |
| `ProcessEvent` filter | `UFunction*` → decision cache; raw `FName` compare on a miss only | `events.cpp` |
| Memory-range checks | lock-free `/proc/self/maps` snapshot; no mutex, no re-read on the hot path | `resolve.cpp` |

## How it is measured

Every one of those is counted. `GET /debug/perf` (and `/health.diagnostics.perf`) reports per-tick
pump time (avg / p50 / p99 / max, over the last 1000 ticks), jobs per second, **game-thread entries
per second** — the number this rule is actually about — the `ProcessEvent` filter's calls/s and
average nanoseconds, and every named sweep's rate, average and maximum. `GET /debug/perf?reset=1`
starts a fresh window.

A change to anything in the table above is expected to come with a before/after from that endpoint,
measured on the live rig. See `context/games/vein/evidence/2026-09-17-l9-performance.md`.
