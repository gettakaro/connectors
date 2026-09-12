---
name: takaro-zomboid-engineer
description: "Takaro Project Zomboid connector knowledge — a ByteBuddy -javaagent inside the PZ B42 server JVM, Gradle (JDK 25) build, bytecode-first hook workflow, and dev-servers deployment/testing patterns."
---

# Takaro Project Zomboid Engineer

Project Zomboid Build 42 in-game connector implementing the Takaro Generic
Connector Protocol. It is **not** a Lua mod: B42 server Lua (Kahlua) exposes no
networking and fires no server connect/disconnect/chat events. The connector is
a Java agent (`Premain-Class`) injected into the PZ server JVM with
`JAVA_TOOL_OPTIONS`, instrumenting the game classes with ByteBuddy.

## Quick Reference

| Area | File | Key Command |
|------|------|-------------|
| Stage game jar (compile deps) | `zomboid/scripts/setup-environment.sh` | `just zomboid-setup` |
| Build (tests + shaded jar) | `zomboid/{core,agent}` | `just zomboid-build` |
| Deploy into dev server | `dev-servers/scripts/deploy-connector.sh` | `just zomboid-deploy` |
| Run dev server | `dev-servers/compose/zomboid.yml` | `just zomboid-up` / `just zomboid-logs` |
| Release artifact | `zomboid/scripts/build-release.sh` | `just build-release-zomboid <v>` |
| Re-pin signatures | `zomboid/scripts/dump-signatures.py` | `python3 zomboid/scripts/dump-signatures.py <jar> <class>` |

## Project Structure

```
connectors/
├── zomboid/
│   ├── core/                # Game-agnostic Takaro WS client (io.takaro.zomboid.core); bytecode 21
│   │                        #   lifted from minecraft/core: ping/pong, tolerant gameId, 10s action timeout
│   ├── agent/               # -javaagent (io.takaro.zomboid.agent); bytecode 25 (links v69 game jar)
│   │   ├── TakaroAgent.java         # Premain-Class: config -> hooks -> (first tick) connect
│   │   ├── ZomboidAdapter.java      # GameAdapter: 17 actions
│   │   ├── Pz.java                  # typed facade over the game classes (main-thread only)
│   │   ├── Catalog.java             # pure item/inventory transforms (unit-tested)
│   │   ├── TokenBucket.java         # rate limiter for entity-killed / log (unit-tested)
│   │   ├── MainThreadQueue / Reconciler / PlayerRegistry / BanStore / ConfigLoader
│   │   ├── EventManagerCallback.java# IEventController -> log event
│   │   └── hooks/                   # Bridge + one Advice class per hook
│   ├── scripts/             # setup-environment.sh, build-release.sh, dump-signatures.py, live-proof.md
│   ├── build.gradle.kts settings.gradle.kts version.txt CHANGELOG.md
│   └── _deps/               # staged projectzomboid.jar (gitignored)
└── .claude/skills/takaro-zomboid-engineer/
```

## Workflow Notes

- **Ground every hook change in the jar's bytecode, never memory or the wiki.**
  Re-pin with `dump-signatures.py` (constant-pool/LocalVariableTable parser, plus
  `--code <class> <method>` to disassemble a method's call sequence). To inspect
  what ByteBuddy actually inlined, dump transformed classes with
  `-Dio.takaro.zomboid.libs.bytebuddy.dump=/tmp/bb` (the relocated property).
- **Never edit files under `ZomboidDedicatedServer/`** (the install dir) — SteamCMD
  `app_update 380870 validate` reverts it on every container start. The agent lives
  in the persistent cache-dir mount at `Zomboid/Takaro/`.
- **Every advice body is exactly one `Bridge` call** wrapped in
  `suppress=Throwable.class`; `Bridge` methods wrap their body in try/catch so no
  exception can escape into game code. No static init in `Bridge`/hooks touches
  game classes.
- **All `GameServer.*`/game-state reads run on the main thread** via `MainThreadQueue`,
  drained by the `RCONServer.update()` tick advice. The WS thread only ever sees
  immutable snapshots (`PlayerRegistry`); chat advice never calls `GameServer`.
- The build targets **JDK 25** (agent) because `projectzomboid.jar` is class-file
  v69; a JDK 21 `javac` cannot read it. `core` stays bytecode 21. ByteBuddy is
  relocated under `io.takaro.zomboid.libs.bytebuddy`.
- **Gotcha — premain runs 3×:** the image launches three JVMs, so `premain` runs
  three times. The connector is started exactly once, from the **first
  `RCONServer.update()` tick**, which is guaranteed to be the real server JVM on
  its main thread with the game classes loaded.
- **Gotcha — RCONServer loads late:** hooks on classes loaded late (RCONServer,
  ChatServer, IsoZombie) bind only once the class loads; `AgentBuilder.Listener`
  firing is *not* proof a matcher bound — rely on the first-invocation sentinels in
  `Bridge` (`HOOK CONFIRMED: …`) and the tick watchdog.
- After a PZ update, re-stage the jar and re-pin signatures; the staged-jar sha is
  checked in `setup-environment.sh` and WARNs on drift.
- **Keep `context/games/project-zomboid/capabilities.json` (gamingconnectors repo)
  current** when action/event coverage changes, with an evidence file per cell.
