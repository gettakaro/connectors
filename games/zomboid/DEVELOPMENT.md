# Project Zomboid connector — development

Developer and architecture notes for the Takaro Project Zomboid connector. Operator-facing
install instructions live in [README.md](README.md).

## Overview

A server-side-only Java agent for Project Zomboid Build 42 dedicated servers that connects to
Takaro through the Generic Connector Protocol over WebSocket. It loads into the server JVM with
`-javaagent`, hooks game methods for events, and runs Takaro actions on the game main thread. No
client-side mod and no Workshop item are required.

## Quick start

From the monorepo root:

```sh
just zomboid-setup      # stage projectzomboid.jar as a compile reference
just zomboid-build      # unit tests + shaded -javaagent jar (needs JDK 25)
just zomboid-deploy     # build and copy the agent into dev-servers/_data
just zomboid-up         # start the dev server with the agent attached
```

Or from inside `games/zomboid/`:

```sh
./scripts/setup-environment.sh
(cd mod && ./gradlew build)
```

The build produces `games/zomboid/mod/agent/build/libs/TakaroConnector-<version>.jar`. Local
server files and build outputs live under `dev-servers/_data/zomboid/`.

PZ B42 ships Zulu OpenJDK 25 and `projectzomboid.jar` is class-file v69, which a JDK 21 `javac`
cannot read. `core` targets bytecode 21, `agent` targets 25.

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
   `-javaagent:<cachedir>/Takaro/TakaroConnector.jar`. Zero file edits, and it survives SteamCMD
   `validate` (which only touches the install dir, never the cache dir). This is the path the
   dev server and the live proofs use.
2. `vmArgs` in `ProjectZomboid64.json` — works, but SteamCMD `validate` reverts it; re-apply
   after game updates.
3. `-javaagent:<jar> --` as a launch option before the `--` separator.

## Configuration

Config is read from `<cachedir>/Takaro/TakaroConfig.txt` (`key=value`), each key overridable by a
`TAKARO_*` environment variable (env wins):

```
wsUrl=wss://connect.takaro.io/
identityToken=
registrationToken=
debug=false
logEvents=false
serverChatName=
```

Env overrides: `TAKARO_WS_URL`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_REGISTRATION_TOKEN`,
`TAKARO_DEBUG`, `TAKARO_LOG_EVENTS`, `TAKARO_SERVER_CHAT_NAME`, `TAKARO_DEBUG_CATALOG`.

The config file path itself can be redirected with the `takaro.configFile` system property, and
the log file with `takaro.logFile` (defaults: `/home/steam/Zomboid/Takaro/TakaroConfig.txt` and
`/home/steam/Zomboid/Takaro/takaro-agent.log`).

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

- `scripts/setup-environment.sh` — stages `projectzomboid.jar` into `_deps/` (bind mount →
  running container → SteamCMD app 380870), with a pinned sha256 for the live-proven build.
- `scripts/dump-signatures.py` — self-contained class-file parser for re-pinning the ByteBuddy
  hooks after a PZ update, including a `--code` disassembler for copying a command class's exact
  call sequence.
- `scripts/takaro-oracle.py` — drives the Takaro REST API to inspect/prove connector actions.
- `scripts/build-release.sh` — builds the shaded `-javaagent` jar at a given version.
- `scripts/live-proof.md` — the client-dependent live-proof checklist.
