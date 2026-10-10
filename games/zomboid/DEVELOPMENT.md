# Project Zomboid connector — development

Developer and architecture notes for the Takaro Project Zomboid connector. Operator-facing
install instructions live in [README.md](README.md).

## Overview

A server-side-only Java agent for Project Zomboid Build 42 dedicated servers that connects to
Takaro through the Generic Connector Protocol over WebSocket. It loads into the server JVM with
`-javaagent`, hooks game methods for events, and runs Takaro actions on the game main thread. No
client-side mod and no Workshop item are required.

## The target

This connector is built for one exactly pinned server build, declared in
`catalog/zomboid/targets/linux-42.20.4.json`: Steam app 380870, branch `public`, build
24909836, the three depot manifests that make up a Linux install, the sha256 of
`java/projectzomboid.jar`, the bundled Zulu JDK 25, the server image by digest, the JDK
toolchain image by digest and every build dependency by sha256.

Everything that needs to know which build this is asks the catalog for it:

```sh
maintenance/bin/takaro-maint targets resolve --game zomboid --format env --prefix ZOMBOID
```

The scripts, the rig (`dev-servers/lib/games/zomboid.sh`, `dev-servers/compose/zomboid.yml`)
and `.github/workflows/zomboid.yml` all consume those `ZOMBOID_*` keys. No script, compose
file or workflow carries a game build, an image tag, a jar hash or an artifact name of its own.

**There is no implicit source of compile references.** The only source of
`projectzomboid.jar` is `takaro-maint steam references`, which downloads it from the pinned
depot manifests. A bind mount, a running container and a SteamCMD branch head are not sources:
none of them can say which build it just handed over. A reference jar whose sha256 is not the
target's exits 5, with no override and no warning-only path.

## Quick start

From the monorepo root:

```sh
just zomboid-setup      # stage the pinned projectzomboid.jar as a compile reference
just zomboid-build      # unit tests + shaded -javaagent jar
just zomboid-deploy     # build and deploy the agent into dev-servers/_data
just zomboid-up         # start the dev server with the agent attached
```

Or from inside `games/zomboid/`, per catalog target:

```sh
./scripts/setup-environment.sh --target linux-42.20.4            # the pinned game jar
./scripts/test.sh --target linux-42.20.4                         # core + agent JUnit suites
./scripts/build-release.sh 1.0.2 /tmp/dist --target linux-42.20.4 # the release jar
```

Gradle always runs inside the target's pinned JDK image (`build.toolchain`), so there is no
host JDK to install and no host-JDK branch to keep in step. PZ B42 ships Zulu OpenJDK 25 and
`projectzomboid.jar` is class-file v69, which a JDK 21 `javac` cannot read; `core` targets
bytecode 21, `agent` targets 25.

### What lands where

| Path | What |
|---|---|
| `games/zomboid/_data/references/<fp16>/` | the pinned `projectzomboid.jar`, scoped by target fingerprint |
| `games/zomboid/_data/dist/<fp16>/` | the built agent jar for that fingerprint |
| `games/zomboid/_deps/projectzomboid.jar` | a symlink into the reference directory, for a bare `./gradlew build` |
| `<dest>/.takaro/installed-target.json` | the ledger: which target a server directory holds |
| `<dest>/.takaro/steamcmd-stub/` | the SteamCMD stub (see below) |

All of it is ignored by git.

## Verifying a target

```sh
maintenance/bin/takaro-maint build  --game zomboid --target linux-42.20.4 \
    --version <v> --out /tmp/dist --toolchain container
maintenance/bin/takaro-maint verify --game zomboid --target linux-42.20.4 \
    --artifacts /tmp/dist --out /tmp/reports --run-id local --startup-timeout 900 \
    --checks build,startup,identify,heartbeat,players,agent-load,pinned-install,hooks-bound,catalog,rcon,action,reconnect,stop
```

The run installs the pinned build into a temporary directory, deploys the jar, boots the
server image and drives it from a fake Takaro. It reaches report level `startup`.

Two things the run does that a reader would otherwise have to guess at:

- **The depots deliver every file 0644.** Steam records which files are executable, but a
  tree taken straight from the manifests has no executable bit anywhere, and the image's
  entrypoint cannot then run `start-server.sh`. The install restores the bit on what the
  files are — ELF images and shebang scripts — rather than on a list of names.
- **A verification run sets `USE_STEAM=false`.** The rig runs a Steam-visible server; a
  verification run is not one, and a server that cannot reach the Steam master servers
  shuts itself down ("Failed to connect to Steam servers"). `-nosteam` touches nothing the
  checks look at: RCON, the hooks and the WebSocket are all inside the JVM.

Four base checks are deliberately not in that list, because each reads something this game
does not write:

| Base check | Why it is excluded | Replaced by |
|---|---|---|
| `connector-load` | reads `inputs.loader.loaderVersion`, a Fabric-shaped field | `agent-load` (the agent's own `target-check` line) |
| `catalog-items`, `catalog-entities` | spot-check Minecraft registry ids | `catalog` (`Base.Axe` → "Axe", `Zombie` → "Zombie") |
| `console` | sends `say`, which is not a Project Zomboid console command | `rcon` (`players` → `Players connected (0)`) |
| `shutdown` | waits for the container to exit; this image's entrypoint owns the exit | `stop` (quit lines, then `docker stop`) |

Two checks exist only for this game: `pinned-install` (the SteamCMD stub spoke and SteamCMD
never ran) and `hooks-bound` (every hooked class is instrumented and the tick hook fires).
Four classes are transformed as they load and the listener says so; `IsoZombie` and
`ZLogger` are already loaded when `premain` runs, so they prove themselves differently —
the watchdog retransforms `IsoZombie` and announces it, and `ZLogger`'s advice announces
itself the first time the server writes a log line.

`catalog` asks for human display names and states the limit rather than pretending there
is none: Build 42 gives a few hundred of its ~5 000 item scripts — wounds, blood decals,
zombie damage overlays — no display name at all, and the connector reports those by their
script id. The check records how many and fails when the share passes 10%, which is what a
broken display-name lookup would look like. Every row a player can hold is named
(`Base.Axe` → "Firefighter Axe").

### Coverage limits

`hooks-bound` proves the player-lifecycle hooks — connect, disconnect, chat, death,
zombie-killed — are **bound, not fired**. Firing them needs a connected game client, which an
unattended run does not have, so the check says so in its own detail and the report carries the
same sentence. The last recorded client evidence is the 2026-09-13/16 hard tests, which stand
for the connector, not for this pin.

## The runtime target guard

`TargetGuard` runs first in `premain`. It reads `META-INF/takaro-target.json` out of its own
jar, finds `projectzomboid.jar` on the JVM's class path, hashes it, and logs exactly one line:

```
[Takaro] target-check: {"result":"ok","policy":"enforce","target":"linux-42.20.4",...}
```

`TAKARO_TARGET_POLICY` decides what a mismatch means: `enforce` (default) installs no hooks at
all, `warn` continues after saying so, `off` does not hash. Two results are never a refusal:
`unpinned` (a jar built without a target — a developer's own build) and `no-game-jar`
(`premain` runs three times per boot and only the last JVM loads the game).

## The rig

`dev-servers/scripts/install.sh zomboid` installs the pinned build into
`dev-servers/_data/zomboid/server` — no first boot, no SteamCMD, no branch head — and
`start.sh` refuses to start a directory that does not hold that target.

The server image runs `steamcmd.sh +runscript install_server.scmd`
(`app_update 380870 … validate`) on every start and exposes **no** environment knob to turn it
off. So the install writes a stub `steamcmd.sh` under `<dest>/.takaro/steamcmd-stub/`, and the
compose file mounts that directory read-only over the image's own steamcmd directory: the
update step then runs the stub, prints one line and exits 0. Because the install directory is
no longer reverted, the agent lives in it (`Takaro/TakaroConnector.jar`) rather than in the
cache directory.

Migrating the live rig on neo is not done by this change. When you want to:

```sh
dev-servers/scripts/stop.sh zomboid \
  && mv dev-servers/_data/zomboid/server dev-servers/_data/zomboid/server.pre-catalog \
  && dev-servers/scripts/install.sh zomboid
```

The old agent jar in `dev-servers/_data/zomboid/config/Takaro/` is simply ignored once
`JAVA_TOOL_OPTIONS` points at the install directory.

## Re-pinning after a Project Zomboid update

```sh
maintenance/bin/takaro-maint steam pin --game zomboid --target linux-42.20.4 --metadata
```

reports which depot manifests moved. Do not re-pin this target to the new head: the ByteBuddy
hooks are pinned to 42.20.4 signatures. Re-pin the hooks with `scripts/dump-signatures.py`
first, then add a new target file for the new build.

## Architecture

Project Zomboid's Lua (Kahlua) sandbox exposes no networking and fires no server-side
connect/disconnect/chat/death events, so the connector is a **Java agent inside the dedicated
server JVM**, not a Lua mod:

- A `premain` agent installs [ByteBuddy](https://bytebuddy.net/) `Advice` hooks on the server
  classes and opens the outbound WebSocket to Takaro.
- **Events** are method hooks: `GameServer.receivePlayerConnect` / `disconnectPlayer` (with a
  `GameServer.Players` reconciler as the source of truth), `ChatServer.sendMessage`,
  `IsoPlayer.onKilled` (death), `IsoZombie.onKilled` (entity-killed), and a `ZLogger` /
  `EventManager` `log` source.
- **Actions** (`getPlayers`, `giveItem`, `teleportPlayer`, `banPlayer`, …) are marshalled onto
  the game main thread via a queue drained from a per-tick hook, then answered/executed; console
  commands go through `GameServer.rcon`.
- The agent shades and relocates its dependencies (ByteBuddy, Java-WebSocket, Gson) so it is
  safe to load alongside the game and other agents.
- `IsoZombie` loads at server boot and slips past the on-load transformer, so a watchdog
  explicitly `retransformClasses()` the late types.

Because it runs on the JVM, this connector requires editing how the server starts (a JVM
argument), the same install tier as Rust's Carbon preload or Valheim's BepInEx.

## Injection paths

Drop `TakaroConnector.jar` in the server cache dir (`<cachedir>/Takaro/`) and inject it into the
server JVM. In order of preference:

1. **`JAVA_TOOL_OPTIONS`** env var (Docker / self-hosted):
   `-javaagent:<jar>`. Zero file edits. Putting the jar in the cache dir
   (`<cachedir>/Takaro/`) survives a SteamCMD `validate`, which is what an unpinned server
   needs; the dev rig instead pins the install and stubs the updater, and keeps the jar in
   `<installdir>/Takaro/`. This is the path the dev server and the live proofs use.
2. `vmArgs` in `ProjectZomboid64.json` — works, but SteamCMD `validate` reverts it; re-apply
   after game updates.
3. `-javaagent:<jar> --` as a launch option before the `--` separator.

## Configuration

The connector folder is the folder that holds the agent jar (`<Zomboid>/Takaro/`), resolved by
`TakaroPaths`. An install whose files (`TakaroConfig.txt`, `TakaroConfig.saved.txt` or
`bans.json`) already sit in `~/Zomboid/Takaro` or `/home/steam/Zomboid/Takaro` keeps that folder
wherever its jar is. `-Dtakaro.configFile`, `-Dtakaro.logFile` and `-Dtakaro.bansFile` override
each file.

Two files, merged per field by `ConfigFiles.resolve`:

| File | Written by | Holds |
|---|---|---|
| `TakaroConfig.txt` | the release zip (template in `agent/src/main/resources/io/takaro/zomboid/agent/`), the owner | every key; the file people edit |
| `TakaroConfig.saved.txt` | the connector only (mode 0600) | `registrationToken` and `identityToken` in use, from the files, never from the env |

- Tokens: `TAKARO_*` env, else `TakaroConfig.txt`, else the saved copy. Every other key: env,
  else `TakaroConfig.txt`. The registration token is never written into `TakaroConfig.txt`.
- Identity: when no source holds one, a random UUID is generated on the first tick and written
  into both files. An identity the saved copy holds is written back into a `TakaroConfig.txt`
  that an upgrade replaced. Takaro refuses an empty identity ("No identityToken provided"), so an
  install without one never had a server record and a new UUID orphans nothing.
- Phases, because `premain` runs in all three JVMs of the launch chain: `load()` in premain only
  reads; `prepareForStart()` on the first tick (real server JVM) writes the template when there
  is no file and no token anywhere, the identity and the saved copy; then a daemon thread polls
  `TakaroConfig.txt` every 5 s (`poll()`), compares the text, and applies a change only when a
  second read 300 ms later sees the same text. A missing or truncated file falls back to the
  saved tokens, so it keeps the current connection.
- Rewrites are atomic (tmp + move) and keep the file's owner and mode (a new file takes its
  folder's owner), so a server running as root does not lock the owner out of the file.
- `TakaroConnector` serialises connect, `applyConfig` and shutdown on one lock. A changed URL or
  token retires the current socket (`TakaroWebSocketClient.retire()`: no reconnect, no identify,
  no events) and connects at once, skipping backoff. No registration token: a `*` banner naming
  the file, no socket. A rejected identify: a banner, the socket is closed and retried with
  backoff (Takaro keeps an unidentified socket open). Close codes 1008/4001/4003 stop retrying
  until the file changes.

Env overrides: `TAKARO_WS_URL`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_REGISTRATION_TOKEN`,
`TAKARO_DEBUG`, `TAKARO_LOG_EVENTS`, `TAKARO_SERVER_CHAT_NAME`, `TAKARO_DEBUG_CATALOG`.

The release has two artifacts: the agent jar (what `takaro-maint deploy` installs as
`Takaro/TakaroConnector.jar`) and the bundle zip (`Takaro/TakaroConnector.jar` +
`Takaro/TakaroConfig.txt`, written by `build-release.sh` with fixed timestamps so two builds are
byte-identical; a deploy parks it in `takaro-bundle/`).

`debugCatalog` logs the `listItems`/`listEntities`/`listLocations` sizes once at start — a
server-side way to prove the catalogue when no Takaro REST driver is available.

## Takaro coverage

**22 of 23** capabilities (17 actions + 6 events) are `live-supported`, verified end to end
through the Takaro API against a real dedicated server with a live client (game build
42.20.4 b0bbce05d5). `listLocations` is `partial`: Build 42 exposes no named-location registry,
so the connector reads `SafeHouse.safehouseList` reflectively and returns `[]` until players
claim safehouses. Full per-capability evidence lives in the campaign docs under
`context/games/project-zomboid/` of the gamingconnectors workspace.

## Chat sender name

Messages the connector sends to game chat are prefixed with a sender name, resolved: Takaro's
`opts.senderNameOverride` on `sendMessage`, else the connector's `serverChatName` config, else
the live PZ server name (`GameServer.serverName`), else `"Server"`.

**Known issue:** Takaro does not currently attach the domain **Server Chat Name** setting to
admin/dashboard messages — they arrive with empty `opts`, and the connector receives no settings
on identify, so those messages fall back to the server name. The connector honors the name
whenever Takaro sends it (module/command flows). Proper fix is Takaro attaching the name to
admin messages; setting `serverChatName` on the connector is a stopgap that duplicates the
Takaro setting, so it is left unset by default.

## Tooling

- `scripts/lib-target.sh` — resolves the catalog target into `ZOMBOID_*` keys; sourced by
  every script here.
- `scripts/lib-gradle.sh` — runs Gradle inside the target's pinned JDK image and builds the
  `-P` properties that carry the target's identity into the jar.
- `scripts/setup-environment.sh` — fetches the pinned `projectzomboid.jar` from the pinned
  depot manifests and refuses anything whose sha256 is not the target's (exit 5).
- `scripts/test.sh` — the `core` and `agent` JUnit suites against that jar.
- `scripts/dump-signatures.py` — self-contained class-file parser for re-pinning the ByteBuddy
  hooks after a PZ update, including a `--code` disassembler for copying a command class's exact
  call sequence.
- `scripts/takaro-oracle.py` — drives the Takaro REST API to inspect/prove connector actions.
- `scripts/build-release.sh` — builds the shaded `-javaagent` jar at a given version.
- `scripts/live-proof.md` — the client-dependent live-proof checklist.
