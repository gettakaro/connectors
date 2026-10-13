# Dragonwilds connector — development

Operator documentation is in [README.md](README.md) and [INSTALL.md](INSTALL.md). This file is for
people who build, change or re-verify the connector.

## Layout

```
mod/       C++17, builds libtakaro-dragonwilds.so (LD_PRELOAD into the dedicated server)
  src/       game side (sym, reflect, gamethread, hooks, actions, events, state) and the native
             Takaro connector (transport, bridge, persistence, log tail)
  docs/      API.md — the optional loopback diagnostic HTTP API
  tests/     host unit tests and native connector tests against a fake Takaro
  third_party/  pinned native dependency notes and license texts (shipped in the release)
  build.sh, Dockerfile.build, Makefile
scripts/   build-release.sh, drain-legacy.py (0.2.x sidecar drain + state import), lib-target.sh,
           check-exact-source.mjs
Dockerfile.builder   release builder: catalog toolchain + catalog-pinned native sources
docker-compose.example.yml, .env.example, INSTALL.md, version.txt, CHANGELOG.md
```

## Architecture

```
RSDragonwildsServer-Linux-Shipping (UE 5.6.1, Linux dedicated server)
  └─ LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so
       ├─ resolves engine/game functions by demangled name from the depot's own .sym (never fixed RVAs)
       ├─ reads every UPROPERTY offset via UStruct::FindPropertyByName at runtime
       ├─ hooks PostLogin / OnNetCleanup / PreLogout / Logout / ProcessEvent by vtable slot swap
       ├─ marshals all UObject work onto the engine tick (bounded job queue, TAKARO_TICK_BUDGET_US)
       ├─ native Takaro connector on background threads:
       │    TLS WebSocket (identify, heartbeat, reconnect), action dispatch, durable event outbox,
       │    online-player reconciliation, timed-ban expiry, server-log tail (secrets redacted)
       └─ optional authenticated diagnostics on 127.0.0.1:18890 (TAKARO_PLUGIN_TOKEN)
```

Networking, JSON, filesystem work and log matching never run on the game thread; hooks enqueue owned
data and the game-thread pump stays inside its tick budget. The rule and its budget are in
`mod/docs/gamethread-policy.md`; read it before adding anything that touches the game thread.

Connector state lives in `TAKARO_STATE_DIR`: the durable `event-outbox.json` and `ban-intent.json`
plus the 0.2.x sidecar's `event-cursor.json`, `online-players.json`, `known-players.json` and
`timed-bans.json`, read in place with the same formats. The game-enforcement `bans.json`,
`symcache.json` and `plugin.log` stay in the plugin data dir (`<exe dir>/takaro`).

## Configuration model

`mod/src/config_file.cpp`. One file: `takaro.cfg` next to the library (`TAKARO_CONFIG_FILE`
overrides the path). Every setting is a non-empty environment variable, else that file; the
environment is never written to disk. The release does not ship the file.

- The init thread, before anything reads a setting, creates the file when it is missing (all
  connection keys, empty token, a new UUID identity, name `Dragonwilds (<first 8>)`, mode 0600), or
  writes a new identity into it when `TAKARO_IDENTITY_TOKEN` is empty (atomic, owner and mode kept).
  Nothing else is written.
- Only the connection settings (URL, token, identity, name) change at runtime: the bridge thread
  re-reads the file every 5 s, applies a changed text once the next read (1 s later) agrees, skips a
  file with a line without `=`, and calls `NativeTransport::Retarget`. The service thread kills the
  old socket (`LWS_TO_KILL_ASYNC`) and dials only after its CLOSED callback, without backoff; frames
  of the old epoch are dropped, so only the answer to the latest identify counts.
- No token, or no identity because the folder is read-only: no connection and a console banner
  (one stdout write) naming the file. An emptied token disconnects.
- A rejected identify logs only the error's name, message and HTTP status; Takaro's error can carry
  its internal request with an `x-takaro-token` JWT, and long token-like runs are redacted.

## Build and test

```bash
./mod/build.sh --tests                     # debian:bookworm container, pinned static deps, all tests
python3 -m unittest discover -s scripts -p 'test_*.py'   # drain-legacy
./scripts/build-release.sh 0.0.0-dev dist  # takaro-dragonwilds-plugin-<target>-<version>.tar.gz
```

The release is built per catalog target (`catalog/dragonwilds/targets/`). `build-release.sh` resolves
the target, builds `Dockerfile.builder` from the target's toolchain image and its `build.deps`
(OpenSSL, libwebsockets, PCRE2, nlohmann/json, each checked by SHA-256), runs
`check-exact-source.mjs` so `mod/Dockerfile.build` cannot drift from those pins, then builds and
tests the plugin and packages it with `pkg_tar_gz`. Bump a dependency in both files together.

The toolchain is Debian Bookworm on purpose: it matches the glibc of the dedicated-server image, so
the `.so` loads without a symbol-version error. `build.sh` refuses a `.so` with undefined strong
symbols, because `LD_PRELOAD` would crash-loop the server.

`DEBUG_CORRUPT_SIG=<symbol> ./mod/build.sh` builds a `.so` with one resolution deliberately broken.
That build must still load, keep the server running, and report exactly that capability as
`degraded` in `/health` — it is how the degrade path is proven.

The plugin version is a compile-time constant in `mod/src/common.h`
(`// x-release-please-version`); release-please bumps it together with `version.txt` and the
changelog. Never hand-edit it.

## Symbol resolution (`.sym`) and reflection

The Dragonwilds depot ships `RSDragonwildsServer-Linux-Shipping.sym` next to the stripped server
binary. Format: `u32 N`, then N × 20-byte records `{u64 rva, u32 line, u32 fileOff, u32 nameOff}`
sorted by rva, then a `\n`-separated table of demangled names. **`vaddr = rva + the first
PT_LOAD p_vaddr`**, read from the ELF at runtime — never hard-coded.

The resolved set is cached in `<serverdir>/takaro/symcache.json`, keyed by the ELF
`.note.gnu.build-id`, so a game update invalidates the cache automatically and the plugin
re-resolves on the next boot. Before any resolved address is called, boot self-checks must pass:
`_init`/`_fini` against the section addresses, `UObject::ProcessEvent` appearing exactly once in
`_ZTV7UObject` (which also yields its vtable slot), every address inside `.text` and not
`0x00`/`0xCC`, and `FindFunction(CDO, "Server_SendChatMessage")->Func == sym(execServer_SendChatMessage)`.
All UPROPERTY offsets come from `FindPropertyByName` at runtime; only the UE 5.6 fixed struct
facts are constants, and each is validated at boot.

Debug endpoints (Bearer token **and** `TAKARO_PLUGIN_DEBUG=1`):

| Endpoint | Use |
|---|---|
| `GET /debug/symbols` | what resolved, how, from cache or a fresh parse |
| `GET /debug/gamethread` | job queue depth, tick hook state, last tick age |
| `GET /debug/object?ptr=…\|path=…` | dump a live UObject's property tree — the discovery tool for new fields |
| `GET /debug/structs?name=…` | resolved struct layout (e.g. `ChatMessageData`) |
| `GET /debug/nearby` | actors around a player |
| `POST /debug/kill-nearest` | drive a creature kill through the game's damage pipeline (entity-killed proof without a human) |

`GET /health` is the operator-facing view of the same thing: plugin version, game build, engine
version, per-capability `ok`/`degraded`, `diagnostics.resolved[{name, rva, how, hooked, fired}]`
and the symcache state. A capability must self-report from `hooked`, not from symbol resolution —
reporting `ok` while the hook never bound was a real bug (a kill hook said `ok` with `hooked:false`).

## After a game update

1. The game's `.sym` ships with the update, so start the server with the plugin and read `/health`.
   Everything that still resolves keeps working; anything that does not is `degraded` and is also
   listed in Takaro's reachability reason. The server itself never fails to start because of this.
2. `GET /debug/symbols` shows which names disappeared or moved. UE/engine names are stable across
   patches; game-specific ones (`ADominion*`, `UDominion*`) are the ones that get renamed.
3. Re-verify the hooks that fire from player actions (`hooked`/`fired` counters in `/health`) with a
   real client join, one chat line, one death and one creature kill — hooks bind to the *live*
   object's vtable, and a new subclass can make a hook silently stop firing.
4. Run `./mod/build.sh --tests`; the native connector tests pin the Takaro wire shapes.

## Degrade semantics

Resolution and validation failures degrade one capability, they never crash the server and never
abort load. Concretely: the boot validation for a feature fails → that capability is marked
`degraded` with a reason → the connector answers that action with a failure naming the reason, and
the reason surfaces in Takaro's reachability text. Every handler is
wrapped in `try/catch(...)` with a readable-memory guard. Never call `FName::ToString` on an
unvalidated `FName`: that crash-looped the server during development.

## Dev rig

The connector is developed against a disposable dedicated server in the repo's `dev-servers/`
harness (its own world, port 7797, auto-update off because client and server are version locked).
`dev-servers/scripts/deploy-connector.sh dragonwilds` builds the `.so`, refuses undefined symbols,
and swaps it into `_data/dragonwilds-dev-plugin` under the rig lock (stop, swap, start). Connector
state is `_data/dragonwilds-dev-state`. It refuses to deploy while the legacy sidecar container
still exists. Never use `_data/dragonwilds-plugin`: a server outside this repository mounts it.
Never test against a server anyone plays on: several cells (ban, shutdown, restart) are destructive.

## Map image (operator upload)

The server ships no map art: its cook strips texture pixels, so `T_WorldMap*` exist only as
metadata. A map can still be derived from the server's own collision and given to Takaro as an
operator upload (`PUT /gameserver/{id}/map/{dimension}/upload`, which cuts the tile pyramid):

1. Stop the server and add to `RSDragonwilds/Saved/Config/LinuxServer/Engine.ini` (back it up first):
   `[SystemSettings]` / `wp.Runtime.EnableServerStreaming=0`. The cvar is read-only at runtime;
   without it the server only loads World Partition cells near players. Start it (about 6.5 GB RAM
   instead of 1 GB once every cell is in).
2. With `TAKARO_PLUGIN_DEBUG=1`, `POST /debug/map/survey {"step":150,"minX":-20000,"maxX":600000,
   "minY":-160000,"maxY":480000,"zTop":200000,"zBottom":-100000,"perJob":2048}` traces the world
   from above (no bounds: the union of loaded landscape proxies, which also covers far-away
   interior sub-levels). Poll `GET /debug/map/survey`. Build 25808123: 4134x4267 samples, about
   135 s of game-thread time spread over about 9 minutes; each job holds the game thread for one
   chunk of a row, so expect hitches while a player is online.
3. `scripts/render-map.py <serverdir>/takaro/map out.png --sea-level -3400` (numpy + Pillow)
   writes the PNG and `out.settings.json` with the axes (x right, y down: north up, matching the
   in-game map) and the bounds in raw Unreal units, which is what Takaro positions use.
4. Upload both as `image` and `settings`, then restore `Engine.ini` and restart.

## Gotchas

- `LD_PRELOAD` goes on the **game binary only** — SteamCMD is 32-bit and fails with it set. Patch
  exactly the server launch line of the image's entrypoint and fail the build if it does not match.
- SteamCMD `validate` wipes the Steam tree: keep the `.so` outside it, mounted read-only.
- The game rewrites `DedicatedServer.ini` on shutdown, so ban edits go through
  `SetBannedUsers` + `PerformConfigSave`, never a text edit; the running server's login check uses a
  start-up snapshot, so the plugin also enforces bans at `PostLogin`.
- The server prints `WorldPassword` in cleartext into its log — redact in the plugin and in
  anything you paste into a report.
- Hooking a base-class vtable does nothing: live objects carry their own vtables (the engine object
  is a `UDomGameEngine`). Hook the live object's vtable and verify `fired` before believing it.
- Takaro modules send explicit JSON `null` for optional arguments (`dimension`, `reason`,
  `expiresAt`, `quality`, `opts`) where the API docs simply omit the key. Handle absent, `null` and
  wrong type.
- Player-mutating actions arrive with a full nested `player` object, not a flat id.
