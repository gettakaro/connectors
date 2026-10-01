# libtakaro-dragonwilds — native server plugin and Takaro connector (dev notes)

An `LD_PRELOAD` shared object for the RuneScape: Dragonwilds dedicated server
(`RSDragonwildsServer-Linux-Shipping`, UE 5.6.1). It is the whole connector: it hooks the game,
holds the Takaro WebSocket itself (outbound `wss://`) and keeps its state durable on disk. No
sidecar. The contract is in `docs/API.md`, state files in `docs/native-state.md`, the game-thread
rules in `docs/gamethread-policy.md`.

This is our own code, built on the Takaro Enshrouded plugin skeleton ported to Linux/POSIX, with the
native Takaro layer of the VEIN plugin. Statically linked, pinned dependencies (OpenSSL,
libwebsockets, PCRE2, nlohmann/json) are listed in `third_party/README.md`; their licenses ship in
`third_party/licenses/`.

## Layout
```
src/  common.*      logging (queued, redacted), JSON, config, time, file helpers
      sym.*         ELF reader, .sym resolver, symcache, lock-free /proc/self/maps guard
      reflect.*     UE object model: FName/FString/TArray, FindFunction, property walks, dumps
      hooks.*       vtable slot swaps + the resolve/hook/fire registry behind /health
      gamethread.*  engine Tick hook + budgeted job queue
      perf.*        game-thread counters (/health.diagnostics.perf, /debug/perf)
      state.*       capability registry, event ring buffer, plugin ban list (bans.json)
      events.cpp    event sources: join/leave, chat, death, kill, log tail
      actions.cpp   the action handlers (game-thread jobs, background-callable)
      native_transport.*    Takaro WebSocket (libwebsockets + OpenSSL), heartbeat, bounded queues
      native_bridge.*       protocol, requests + answer deadline, durable outbox, scheduling
      native_behavior.*     Takaro shapes (event whitelist), timed bans, ban journal, reconcile
      native_persistence.*  atomic state files (event-outbox.json, ban-intent.json, 0.2.x files)
      native_log.*          RSDragonwilds.log grammar, redaction, noise filter (PCRE2)
      http.*        optional loopback diagnostics (token-gated)
      main.cpp      constructor -> init thread -> bridge start
tools/symdump.py    prints/records the wanted symbol table for a build
tests/              unit tests, game-thread timeout, TLS transport, bridge adversarial,
                    behaviour parity with the 0.2.x sidecar, full bridge against a fake Takaro
docs/               API.md, native-state.md, gamethread-policy.md, symbols-<build-id>.md
```

## Build
```
./build.sh                 # debian:bookworm builder -> dist/libtakaro-dragonwilds.so + SHA256SUMS
./build.sh --tests         # build, then run every test suite (unit, timeout, transport, bridge,
                           # behaviour parity, full bridge)
./build.sh --native        # inside the builder (needs the pinned deps in $TAKARO_NATIVE_PREFIX)
./tests/run.sh             # one suite; also run-timeout.sh, run-transport.sh, run-native-*.sh
make symbols               # regenerate docs/symbols-<build-id>.md from the installed server
DEBUG_CORRUPT_SIG=x ./build.sh   # deliberately broken build, for the degrade proof
```
Flags: `-std=c++17 -O2 -fPIC -fvisibility=hidden`, linked `-shared -static-libstdc++ -static-libgcc`
with the pinned static libraries and `exports.map` (nothing exported). The build refuses an artefact
with undefined strong symbols: `LD_PRELOAD` would crash-loop the server.

## Deploying

The plugin file is mounted/copied read-only outside the Steam tree and `LD_PRELOAD` is applied to
the server launch line only (never to 32-bit steamcmd):

```
cp dist/libtakaro-dragonwilds.so <somewhere outside the Steam tree>/
LD_PRELOAD=<that path>/libtakaro-dragonwilds.so \
  TAKARO_IDENTITY_TOKEN=<identity> TAKARO_REGISTRATION_TOKEN=<registration token> \
  ./RSDragonwildsServer.sh -log
```

Runtime artefacts land in `<server>/RSDragonwilds/Binaries/Linux/takaro/`: `plugin.log`,
`symcache.json`, `bans.json`, the connector state files, optional `plugin.json`. Diagnostics, when
`TAKARO_PLUGIN_TOKEN` is set:
`curl -s -H "Authorization: Bearer $TAKARO_PLUGIN_TOKEN" http://127.0.0.1:18890/health`.

## Rules that cost us a crash
- **Never call `FName::ToString` on an FName you have not validated.** It indexes the engine name
  pool and segfaults on a bogus index. Compare raw FName values instead (see `reflect.cpp`).
- **Hooking a base class vtable is not enough.** Derived classes carry their own copy of an
  inherited function pointer; sweep `_ZTV*` for the address (see `gamethread.cpp`).
- Every game pointer goes through `MemReadable()` before it is dereferenced.
- Nothing that can run off the game thread runs on it (`docs/gamethread-policy.md`).
- A capability that cannot be set up degrades with a reason; the server must never be affected.
