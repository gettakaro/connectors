# Dune: Awakening connector — development

Operator documentation is in [README.md](README.md). This file is for people who build, change or re-verify the
connector.

## Layout

```
sidecar/   Node 22 + TypeScript — the connector. Speaks the Takaro Generic Connector Protocol.
  src/dune/      the battlegroup adapter: pg.ts (read-only roster/inventory/life state),
                 rmq.ts (chat in/out), gm.ts (the GM command bus + publishers),
                 presence.ts / presence_diff (connect/disconnect/death from the roster),
                 bans.ts (connector-owned bans), catalogue.ts, config.ts
  src/takaro/    the protocol side: identify, request/response, gameEvent, delivery confirmation
  src/testing/   mockBattlegroup — an in-process Postgres+RabbitMQ double; fixtures/
  src/__tests__/ the unit + wire suites
  scripts/gen-catalogue.mjs   builds data/items.json (see data/ATTRIBUTION.md)
mod/       C++17, builds libtakaro-dune.so (LD_PRELOAD into the MAP server process only)
  src/       resolve.cpp reflect.cpp uobj.cpp objpool.cpp gamethread.cpp hooks.cpp livehooks.cpp
             http.cpp query.cpp players.cpp presence.cpp presence_diff.cpp events.cpp
             events_parse.cpp perf.cpp state.cpp common.cpp main.cpp
  docs/      API.md — the plugin's HTTP contract (the source of truth for both sides)
             gamethread-policy.md — what may touch the game thread, and its budget
  tests/     host unit tests (JSON, ring buffer, redaction, log grammar, resolver bookkeeping)
  tools/     sigderive.py, vtprobe.py — offline binary inspection
  build.sh, Dockerfile.build, Makefile
scripts/build-release.sh
docker-compose.example.yml, .env.example, version.txt, CHANGELOG.md
```

## Configuration model

`src/dune/config.ts` reads every `DUNE_*` setting from the environment once, at start. The four Takaro connection
settings (`TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_WS_URL`, `TAKARO_SERVER_NAME`) come from
`src/takaro/settings.ts` instead, and can change while the sidecar runs:

- Sources: the environment, and the `.env` file at `TAKARO_CONFIG_FILE` (default `../.env` beside the sidecar folder;
  the compose example mounts its own folder read-only at `/takaro-config` and points there). Only those four keys are
  read from the file.
- Precedence: a non-empty environment value wins at start, as before. The file is re-read every
  `TAKARO_CONFIG_POLL_MS` (5000) and compared by text; once a key's file value differs from what the file said at
  start, the file wins for that key. compose bakes `--env-file` values into the container at create, so this is what
  lets a saved `.env` take effect without `up -d`. A failed read keeps the current settings.
- A changed effective value calls `TakaroWsClient.reconfigure`: drop the socket, reset the backoff, connect at once.
  Late callbacks of the dropped socket are ignored (`this.ws !== ws`); shutdown wins over a pending reconfigure.
- No registration token: no socket, a `TAKARO_REGISTRATION_TOKEN not set` banner. A rejected identify: a banner with
  only the error's name, message and HTTP status (Takaro errors can embed an `x-takaro-token` JWT; the logger also
  redacts JWTs), then retry with backoff.
- Identity: configured value > `TAKARO_DATA_DIR/takaro-identity.json` > `dune` when the data dir already holds state
  from before identities were generated > a new UUID. The result is always saved there. The identity is never
  written into the operator's `.env` (the compose mount is read-only).
- Server name: Takaro uses it only when it creates the record, and names are unique per domain (a deleted record keeps
  its name reserved for a while). The default is `Dune (<first 8 identity characters>)`; a 409 gets its own banner.

**Ownership rule: game truth lives in the plugin, Takaro protocol shape lives in the sidecar.** Never parse Takaro
DTOs in C++; never guess game state in TypeScript. Change `mod/docs/API.md` first, then both sides.

## Architecture

A self-hosted Dune server is a **battlegroup**, not a process: `igw-postgres`, a **game** RabbitMQ, an **admin**
RabbitMQ, a `text-router` (which is also the brokers' HTTP auth backend), a `bg-director`, a `gateway` that registers
the world with Funcom Live Services, and one UE5 map server per map. There is **no RCON**, no kill feed and no
documented log grammar. So the connector is an out-of-process adapter plus an optional in-process plugin:

```
sidecar/ (Node 22)
  ├─ Postgres, READ-ONLY: player_state ⋈ accounts ⋈ encrypted_accounts ⋈ actors for the roster
  │   (gameId = FLS id, steamId = accounts.platform_id), inventories ⋈ items, life state
  ├─ game RabbitMQ: consumes chat from exchange `chat.intercept` (rk `#`); publishes on
  │   `chat.whispers` (rk = the player's funcom id) and the `chat.map` fan-out
  ├─ GM command bus — the ONLY write path: publish to exchange `heartbeats` rk `notifications`
  │   with {"Version":2,"AuthToken":…,"MessageContent":"<inner JSON>"} and AMQP user_id=fls.
  │   Publishers: amqp | docker-exec | kubectl-exec (see README.md)
  ├─ every mutation READS BACK the effect before answering success, else 409 verified:false
  ├─ owns bans (Dune has none): bans.json + kick-on-sight + connector-side expiry
  └─ Takaro: outbound WebSocket, events held until confirmed, persisted cursor
mod/ libtakaro-dune.so — LD_PRELOAD into DuneSandboxServer-Linux-Shipping (UE 5.2.1)
  ├─ the ELF is STRIPPED: no function symbols at all. Resolution is anchored on RTTI
  │  (_ZTV* vtables, ~43k of them) and validated at boot — 37275/37275 class identity matches
  ├─ reads every UPROPERTY offset through the engine's own reflection at runtime
  ├─ hooks ProcessEvent (slot verified once by a fired detour) and PostLogin/OnNetCleanup on
  │  LIVE objects only; the name filter costs 24–30 ns/call
  ├─ no engine Tick is reachable, so the job pump rides ProcessEvent under TAKARO_TICK_BUDGET_US
  └─ HTTP API on 127.0.0.1:18890, Bearer token — see mod/docs/API.md
```

**Identity.** `DunePlayerState` carries no FLS id UPROPERTY, so the plugin cannot name players on its own: it reports
a controller/pawn actor reference and the sidecar joins that to Postgres. The plugin's `PostLogin` persona is the
Funcom **account** name, not the character name — Postgres wins on any disagreement.

## Build and test

```bash
cd sidecar && npm ci && npm run typecheck && npm test
cd ../mod && ./build.sh --tests        # debian:bookworm toolchain container; --native to build on the host
```

The plugin is built on `debian:bookworm` (glibc 2.36) with libstdc++/libgcc linked statically, deliberately: the Dune
server image is Ubuntu 24.04 with glibc 2.39, and building *on* 2.39 would produce a `.so` that refuses to load
anywhere older. `build.sh` refuses to emit a `.so` with an undefined strong symbol — the dynamic loader fails the
**whole process** on a bad preload, which once left a map server crash-looping.

`DEBUG_CORRUPT_SIG=<name> ./build.sh` produces a deliberately broken build; it must degrade exactly one capability in
`/health` and leave everything else working. That is the self-degrade proof.

## Targets and releases

The server build this connector is made for is pinned in `catalog/dune/targets/`, and every script reads it from
there: `scripts/lib-target.sh` resolves it through `maintenance/bin/takaro-maint targets resolve`, so no script here
holds a copy of the Steam app, the depot manifest, the image tag, a dependency URL or an artifact name.

```
maintenance/bin/takaro-maint targets list --game dune --format table
maintenance/bin/takaro-maint build --game dune --target linux-25789279 --version 0.1.0 --out dist
scripts/build-release.sh <version> <out-dir> [--target <id>]     # the same build, directly
```

Both halves are built inside the one image the target pins as its toolchain: `node:*-bookworm` (not slim) carries the
g++ the plugin needs on the same glibc 2.36 as above, so one pinned image covers the TypeScript and the C++ without
either of them being built on a host toolchain. `check-exact-source.mjs` runs first and refuses a lockfile that no
longer resolves to the recorded dependency tarballs. The artifacts are named after the target
(`takaro-dune-sidecar-linux-<build>-<version>.tar.gz`) and each carries a `.meta.json` and a `takaro-target.json`, so
what is installed can always say which build it was made for. The build refuses to ship a generated item catalogue
(see below).

Two things this connector does **not** get from the shared machinery yet, both recorded in the target's notes:
`takaro-maint install --game dune` (the depot is a bundle of container images, and the shared Steam install path has
no `docker load` seam), and `takaro-maint verify` (a world is only joinable with an operator's own Funcom token, and
no hosted runner holds 15 GB of images) — which is why the workflow passes `runtime: false` and the target declares
`verification.required: contract`. Runtime proof is produced on the dev-servers rig against this exact pin.

## The item catalogue

`sidecar/data/items.json` in this repository is a **placeholder** of 15 hand-written rows. The full ~2200-row
catalogue is generated by `sidecar/scripts/gen-catalogue.mjs` from the community wiki API, which is CC BY-NC-SA and
therefore not redistributed here — the Docker build and the entrypoint generate it instead. Read
`sidecar/data/ATTRIBUTION.md` before changing any of that.

The generator's one hard rule: `code` is what the game takes, `name` is what a player reads, and a row that cannot
supply a genuinely human name is **dropped** rather than passed to Takaro as an asset id. Its `isDevName` gate is unit
tested; the tests use fixtures under `src/testing/fixtures/` and never read the generated file.

## Things that will bite you

- **The two GM gates and the `UserGame.ini` allow-list** (README step 3). Without them every GM command is accepted
  by the broker and silently dropped by the game. The connector's read-back turns that into a visible failure.
- **`PlayerId` in a GM command is the FLS id**, not the Steam id or the character name (`DUNE_GM_PLAYER_ID_KIND`).
- **`server_id` is TEXT** in the schema, and the database *name* is build-specific (`dune_sb_1_5_3_0`).
- **Presence is three-state** (`Offline` / `LoggingOut` / `Online`), and a map transfer must not look like a
  disconnect — hence `DUNE_TRANSFER_GRACE_SECONDS`.
- **The chat sender is not ours to choose.** Dune renders it from the AMQP `user_id` of a real player account, so a
  server-sent chat line always shows `[]`. The named path is the on-screen `ServiceBroadcast` panel.
- **Takaro modules send explicit JSON `null`** for optional arguments; every handler must tolerate it (`null-args`
  matrix test).
- **Nothing halts the map process** from inside the battlegroup, so `shutdown` needs an operator hook.
- **The map server logs secrets** (the FLS token and the join password are on the command line UE echoes at boot).
  The log tailer redacts before anything leaves the host, starts at end-of-file, and parses nothing.

## Re-verifying

The capability table in README.md is only as good as its evidence. Every row is backed by a Takaro record plus an
independent game-side check (psql, the broker's counters, a map-server log milestone, the plugin's `/health` and
`/events`, or a client screenshot). Do not promote a row on a `success: true` — the read-back exists because GM
commands can be acknowledged and ignored.
