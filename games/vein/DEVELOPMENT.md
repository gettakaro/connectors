# VEIN connector — development

Operator documentation is in [README.md](README.md). This file is for people who build,
change or re-verify the connector.

## Layout

```
mod/       C++17, builds libtakaro-vein.so (LD_PRELOAD into the dedicated server)
  src/       resolve.cpp reflect.cpp gamethread.cpp hooks.cpp http.cpp actions.cpp
             actions_util.cpp admin.cpp events.cpp events_parse.cpp state.cpp common.cpp main.cpp
  docs/      API.md — the plugin's HTTP contract (the source of truth for both sides)
  tests/     host unit tests (JSON, ring buffer, redaction, resolver bookkeeping, action helpers)
  tools/     symprobe.py, sigderive.py, dwarfoffsets.py — offline binary inspection
  build.sh, Dockerfile.build, Makefile
sidecar/   Node 22 + TypeScript, speaks the Takaro Generic Connector Protocol
  Dockerfile      runtime image for the release tarball (expects a prebuilt dist/)
  Dockerfile.dev  builds from src/ — what the dev rig and `build: ./sidecar` in a
                  source checkout use
scripts/   build-release.sh, render-readme-table.mjs
docker-compose.example.yml, .env.example, version.txt, CHANGELOG.md
```

**Ownership rule: game truth lives in the plugin, Takaro protocol shape lives in the sidecar.**
Never parse Takaro DTOs in C++; never guess game state in TypeScript. Change `mod/docs/API.md`
first, then both sides.

## Architecture

```
VeinServer-Linux-Test (UE 5.6.1, Linux dedicated server, Steam app 2131400)
  └─ LD_PRELOAD=/opt/takaro/libtakaro-vein.so
       ├─ resolves engine/game functions with a strategy chain (symtab -> depot .sym -> dynsym ->
       │  string-xref signature scan); every entry records how it was found
       ├─ reads every UPROPERTY offset via UStruct::FindPropertyByName at runtime (no fixed offsets)
       ├─ hooks PostLogin / PreLogin / OnNetCleanup / ProcessEvent by vtable slot swap on LIVE objects
       ├─ marshals all UObject work onto the engine tick (bounded job queue, timeout -> 503)
       └─ HTTP API on 127.0.0.1:18890, Bearer token — see mod/docs/API.md
sidecar/ (Node 22, TypeScript) — same network namespace as the game server
  ├─ polls /events with a persisted cursor + bootId (no replay after a restart)
  ├─ reconciles the online player set (emits disconnects after a server crash)
  ├─ tails the server log for join/leave grammar and log events (secrets redacted)
  ├─ lifts timed bans itself (Takaro sends no unbanPlayer at expiry)
  └─ outbound WebSocket to Takaro (identify, action request/response, gameEvent)
```

## Build and test

```bash
./mod/build.sh                 # debian:bookworm container -> mod/dist/libtakaro-vein.so
./mod/build.sh --native        # on the host (needs g++ >= 10)
./mod/tests/run.sh             # plugin host unit tests (container; --native for the host)
cd sidecar && npm ci && npm run typecheck && npm test && npm run build
./scripts/build-release.sh     # both, packaged into dist/ with SHA256SUMS
```

The toolchain image is `debian:bookworm` on purpose: it matches the glibc of the dedicated-server
image, so the `.so` loads without a symbol-version error. Flags are
`-std=c++17 -O2 -fPIC -fvisibility=hidden`, linked `-shared -pthread -ldl -static-libstdc++ -static-libgcc`.

`DEBUG_CORRUPT_SIG=<name> ./mod/build.sh` builds a `.so` with one resolution deliberately broken.
That build must still load, keep the server running, and report exactly that capability as
`degraded` in `/health` — it is how the degrade path is proven. The variable is passed **into** the
container by `build.sh`; without that it silently produces an ordinary build.

The plugin version is a compile-time constant in `mod/src/common.h`
(`// x-release-please-version`); release-please bumps it together with `version.txt` and the
changelog. Never hand-edit it.

## Dev rig

The connector is developed against a disposable dedicated server in this repository's
`dev-servers/` harness — its own world, its own ports, auto-update off because client and server
are version-locked:

```bash
dev-servers/scripts/install.sh vein        # SteamCMD anonymous app_update 2131400
dev-servers/scripts/start.sh vein
dev-servers/scripts/deploy-connector.sh vein   # build the .so, swap it in, rebuild the sidecar
dev-servers/scripts/stop.sh vein
```

`deploy-connector.sh vein` builds the `.so` inside the toolchain container, checks it for undefined
symbols before it is ever preloaded, copies it into the host directory that is bind-mounted
read-only into the game container, and recreates the sidecar. The rig's configuration (ports,
identity, tokens, admin SteamID64s, join password) lives in `dev-servers/.env`; see
`dev-servers/.env.example` for the keys. Never test against a server anyone plays on: several
checks (ban, shutdown, restart) are destructive.

### Checking a newly detected Steam build

The release watcher files an issue when Steam's public build changes. That issue is a
request to test; discovery alone does not run a server. On an isolated test host, install
the **exact build named by the issue** into the Vein rig, deploy the connector, start
`vein` and `vein-takaro`, then run:

```bash
python3 games/vein/scripts/smoke-server.py --expected-build <Steam build id> \
  --out reports/vein-smoke.json
```

This read-only gate first checks the Steam app manifest. It then checks the game's HTTP
API, plugin self-checks and capabilities, sidecar registration with Takaro, and the
plugin's player and item lookups. It fails if the installed build differs from the issue,
so an old healthy server cannot be mistaken for proof of a new release. It never prints
the plugin token. A passing report names the build, counts and elapsed time.

For a client-present run, connect a **matching Vein client** to that isolated server and
run the same command with `--require-player`. This additionally requires a player to be
visible both to the game HTTP API and to the plugin. This gate confirms the join path;
it does not prove chat, inventory, item delivery, teleport or event delivery to Takaro.
Those require a scripted client, a controlled test account and assertions against the
Takaro event and action APIs. The September 2026 live-player evidence describes the
actions and game-side checks to reproduce. Store both reports on the maintenance issue;
only call a build compatible after the client actions and events pass.

To run these checks unattended after a release, the isolated game host and a matching
client PC must be registered as dedicated runners. The release issue supplies the build
id, the runner installs that exact depot and connector artifact, and the two gates write
separate reports. Keep the game's auto-update disabled between runs; an update must be
explicit and the manifest must match the issue before either report can pass.

## Symbol resolution and reflection

The Linux depot's server binary is the only thing the plugin can rely on, so resolution is a
chain, tried in order, and each resolved name records *how* it was found:

| Strategy | What it uses |
|---|---|
| `symtab` | a full `.symtab` in the ELF, if the depot ships one |
| `depotsym` | a `.sym` side-file next to the binary (rva + name table; `vaddr = rva + the first PT_LOAD p_vaddr`) |
| `dynsym` | exported dynamic symbols, including the `_ZTV*` vtables |
| `signature` | string-xref signature scans; each signature must match exactly once or it is rejected |

Resolved addresses are cached under the server directory, keyed by the ELF `.note.gnu.build-id`, so
a game update invalidates the cache automatically. Before any resolved address is called, boot
self-checks must pass: `_init`/`_fini` against the section addresses, `UObject::ProcessEvent`
appearing exactly once in `_ZTV7UObject` (which also yields its vtable slot), every address inside
`.text` and not `0x00`/`0xCC`, and `FindFunction(CDO, "<fn>")->Func == the resolved exec thunk`.
All UPROPERTY offsets come from `FindPropertyByName` at runtime; only the UE 5.6 fixed struct facts
are constants, and each is validated at boot.

Debug endpoints (Bearer token **and** `TAKARO_PLUGIN_DEBUG=1`; otherwise `404`):

| Endpoint | Use |
|---|---|
| `GET /debug/symbols` | what resolved, how, from cache or a fresh scan |
| `GET /debug/gamethread` | job queue depth, tick hook state, last tick age |
| `GET /debug/object?ptr=…\|path=…` | dump a live UObject's property tree — the discovery tool for new fields |
| `GET /debug/structs?name=…` | resolved class/struct layout |
| `POST /debug/set-admin` | grant or revoke in-game admin for a SteamID64 |
| `POST /debug/kill-nearest` | drive a creature kill through the game's damage pipeline (entity-killed proof without a human) |

`GET /health` is the operator-facing view of the same thing: plugin version, game build, engine
version, per-capability `ok`/`degraded`, `diagnostics.resolved[{name, rva, how, hooked, fired}]`
and the cache state. A capability must self-report from `hooked`, not from symbol resolution —
reporting `ok` while the hook never bound is a bug that has happened before.

## After a game update

1. Start the server with the plugin and read `/health`. Everything that still resolves keeps
   working; anything that does not is `degraded` and is also listed in Takaro's reachability reason.
   The server itself never fails to start because of this.
2. `GET /debug/symbols` shows which names disappeared or moved. Engine (`UObject`, `AGameSession`,
   …) names are stable across patches; the `AVein*`/`UVein*` ones are the ones that get renamed.
3. Re-verify the hooks that fire from player actions (`hooked`/`fired` counters in `/health`) with a
   real client join and one chat line — hooks bind to the *live* object's vtable, and a new subclass
   can make a hook silently stop firing.
4. Run the sidecar test suite; it pins the Takaro wire shapes, not the game.

## Degrade semantics

Resolution and validation failures degrade one capability; they never crash the server and never
abort load. Concretely: the boot validation for a feature fails → that capability is marked
`degraded` with a reason → the matching HTTP endpoint answers `501`/`503` → the sidecar reports the
action as unsupported and the reason surfaces in Takaro's reachability text. Every handler is
wrapped in `try/catch(...)` with a readable-memory guard.

## Gotchas

- `LD_PRELOAD` goes on the **game binary only** — SteamCMD is 32-bit and fails with it set. Patch
  exactly the server launch line of the image's entrypoint and fail the build if it does not match.
- SteamCMD `validate` wipes the Steam tree: keep the `.so` outside it, mounted read-only. Mount the
  *directory*, not the file, so compose still starts before the `.so` exists.
- **Never call VEIN's `NetMulticast_SendChat` with a null sender** — it dereferences the sender and
  SIGSEGVs the whole server. Broadcasts go through the sender-less multicast entry first and only
  fall back to the chat path when a real player state exists.
- Hooking a base-class vtable does nothing: live objects carry their own vtables. Hook the live
  object's vtable and verify `fired` before believing it.
- Never call `FName::ToString` on an unvalidated `FName`.
- Takaro modules send explicit JSON `null` for optional arguments (`dimension`, `reason`,
  `expiresAt`, `quality`, `opts`) where the API docs simply omit the key. Handle absent, `null` and
  wrong type.
- Player-mutating actions arrive with a full nested `player` object, not a flat id.
- The server prints the join password and players' Steam session tickets into its own log — redact
  in the plugin, the sidecar and anything you paste into a report.

## The README status table

The 49-row "What works, what doesn't" table is generated, not hand-edited:

```bash
node scripts/render-readme-table.mjs <path to capabilities.json> --write README.md
```

The row set is fixed in the script (it matches the Dragonwilds connector's table so the two stay
comparable); only the ✅/⚠️/❌ symbol comes from the campaign's `capabilities.json`. Re-run it
whenever a capability's status changes, and never claim a row works without an end-to-end check on
both the Takaro side and the game side.

## Restarting the game container (F17)

The sidecar runs in the game container's network namespace (`network_mode: service:vein`). Docker gives the game
a **new** namespace on every restart of that container and the sidecar keeps the old, dead one, so it loses the
plugin and DNS at once. Since 2026-09-17 the sidecar handles this by itself: its health probe keeps running after
the Takaro socket drops, and after `SIDECAR_EXIT_AFTER_UNREACHABLE_MS` (45 s) with both the plugin and the game's
own `:8080` unreachable it exits(1); `restart: unless-stopped` re-creates it in the live namespace.

So `docker compose restart vein` now needs **no** follow-up `docker restart <sidecar>` — expect the sidecar to be
back within ~60 s (`docker inspect -f '{{.RestartCount}}'` goes up by one). If you are in a hurry, or if you
deliberately run the sidecar with the watchdog disabled (`SIDECAR_EXIT_AFTER_UNREACHABLE_MS=0`), restart it by
hand as before. Proof of the self-recovery: `evidence/2026-09-17-l4b-sidecar-fixes.md`.

The second watchdog, `SIDECAR_EXIT_AFTER_PLUGIN_LOSS_MS` (default `180000`), exits the sidecar when the plugin
alone has been unreachable for that long while the game's own `:8080` still answers — the shape of a game that
came back without the preload. Both watchdogs are disabled by setting them to `0`.

## Event delivery confirmation (F20)

The Takaro protocol has no per-event acknowledgement, so the sidecar proves delivery with the WebSocket
heartbeat: it pings every **5 s**, every written game event carries a monotonic `sendId`, and a pong releases
everything written before that ping. The persisted cursor (`cursor.json`) advances only on that release, never
on a bare `ws.send()`, and two unanswered pings (~10-15 s) tear the socket down; the whole unconfirmed window
then goes back to the front of the pending queue and is re-sent in order after the next `identifyResponse`.
Neither the ping interval nor the missed-pong budget is configurable by env — they are
`TakaroWsClient` options defaulting to `5_000` / `2` in `sidecar/src/takaro/client.ts`. Trade-off: a pong lost after Takaro stored an event re-sends that event, so a duplicate is
possible within one heartbeat. Proof: `evidence/2026-09-17-l4c-outage-delivery.md`.

## Performance

Measured on the dev rig over three 10-minute windows (no plugin / plugin / plugin after tuning), same
world, one player online: the game thread went from **10.26 %** CPU without the plugin to **10.46 %**
with it, and the plugin's own work inside one server tick averages **~3 µs** — about 0.01 % of the
33 ms tick budget, worst observed tick 2.4 ms. A busy server with many players has never been
measured, so treat these as a floor. `GET /debug/perf` reports the live figures on any server.
