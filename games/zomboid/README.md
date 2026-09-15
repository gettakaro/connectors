# Takaro Project Zomboid Connector

A server-side-only Java agent for Project Zomboid Build 42 dedicated servers that
connects to Takaro through the Generic Connector Protocol over WebSocket. It loads
into the server JVM with `-javaagent`, hooks game methods for events, and runs Takaro
actions on the game main thread. No client-side mod and no Workshop item are required.

## Quick Start

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

## Architecture

Project Zomboid's Lua (Kahlua) sandbox exposes no networking and fires no server-side
connect/disconnect/chat/death events, so the connector is a **Java agent inside the
dedicated server JVM**, not a Lua mod:

- A `premain` agent installs [ByteBuddy](https://bytebuddy.net/) `Advice` hooks on the
  server classes and opens the outbound WebSocket to Takaro.
- **Events** are method hooks: `GameServer.receivePlayerConnect` / `disconnectPlayer`
  (with a `GameServer.Players` reconciler as the source of truth), `ChatServer.sendMessage`,
  `IsoPlayer.onKilled` (death), `IsoZombie.onKilled` (entity-killed), and a `ZLogger` /
  `EventManager` `log` source.
- **Actions** (`getPlayers`, `giveItem`, `teleportPlayer`, `banPlayer`, …) are marshalled
  onto the game main thread via a queue drained from a per-tick hook, then answered/executed;
  console commands go through `GameServer.rcon`.
- The agent shades and relocates its dependencies (ByteBuddy, Java-WebSocket, Gson) so it
  is safe to load alongside the game and other agents.

Because it runs on the JVM, this connector requires editing how the server starts (a JVM
argument), the same install tier as Rust's Carbon preload or Valheim's BepInEx.

## Installation

Drop `TakaroConnector.jar` in the server cache dir (`<cachedir>/Takaro/`) and inject it
into the server JVM. Injection paths, in order of preference:

1. **`JAVA_TOOL_OPTIONS`** env var (Docker / self-hosted): `-javaagent:<cachedir>/Takaro/TakaroConnector.jar`. Zero file edits, and it survives SteamCMD `validate` (which only touches the install dir, never the cache dir).
2. `vmArgs` in `ProjectZomboid64.json` — works, but SteamCMD `validate` reverts it; re-apply after game updates.
3. `-javaagent:<jar> --` as a launch option before the `--` separator.

## Configuration

Config is read from `<cachedir>/Takaro/TakaroConfig.txt` (`key=value`), each key overridable
by a `TAKARO_*` environment variable (env wins):

```
wsUrl=wss://connect.takaro.io/
identityToken=
registrationToken=
debug=false
logEvents=false
serverChatName=
```

Set `registrationToken` from your Takaro game-server connector setup before the server can
identify. `wsUrl` defaults to the production Takaro WebSocket URL. Env overrides:
`TAKARO_WS_URL`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_REGISTRATION_TOKEN`, `TAKARO_DEBUG`,
`TAKARO_SERVER_CHAT_NAME`.

## Takaro coverage

**22 of 23** capabilities (17 actions + 6 events) are `live-supported`, verified end to end
through the Takaro API against a real dedicated server with a live client (game build
42.20.4 b0bbce05d5). `listLocations` is `partial`: Build 42 exposes no named-location
registry. Full per-capability evidence lives in the campaign docs under
`context/games/project-zomboid/` of the gamingconnectors workspace.

### Chat sender name

Messages the connector sends to game chat are prefixed with a sender name, resolved:
Takaro's `opts.senderNameOverride` on `sendMessage`, else the connector's `serverChatName`
config, else the live PZ server name (`GameServer.serverName`), else `"Server"`.

**Known issue:** Takaro does not currently attach the domain **Server Chat Name** setting to
admin/dashboard messages — they arrive with empty `opts`, and the connector receives no
settings on identify, so those messages fall back to the server name. The connector honors
the name whenever Takaro sends it (module/command flows). Proper fix is Takaro attaching the
name to admin messages; setting `serverChatName` on the connector is a stopgap that
duplicates the Takaro setting, so it is left unset by default.
