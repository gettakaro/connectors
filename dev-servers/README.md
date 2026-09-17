# dev-servers

A single Docker Compose environment for running every game server this repo has a
connector for, with the **real Takaro connector or server mod** installed and configured —
not just the base game.

Built for a dedicated test box (Linux, roughly 64 GB RAM and 400 GB of free disk) used
for connector, Takaro API and Takaro module testing with a handful of human players.

> This is a **test** environment. Servers are insecure by design: RCON passwords live in a
> local `.env`, Minecraft runs with `ONLINE_MODE=false`, and Rust runs with
> `server.secure 0`. Do not expose these to the public internet.

## Games

| Game | Id | How Takaro connects | RAM~ | Disk~ |
|---|---|---|---|---|
| Terraria | `terraria` | TShock **REST API** + `TakaroTerrariaEvents` log-marker plugin | 1 GB | 1 GB |
| Minecraft Paper | `minecraft-paper` | Takaro Paper plugin (WebSocket) | 3 GB | 2 GB |
| Minecraft NeoForge | `minecraft-neoforge` | Takaro NeoForge mod (WebSocket) | 3 GB | 2 GB |
| Minecraft Fabric | `minecraft-fabric` | Takaro Fabric mod (WebSocket), pinned by a catalog target | 3 GB | 2 GB |
| Minecraft Fabric 26.1.2 | `minecraft-fabric-26.1.2` | Takaro Fabric mod (WebSocket), pinned by catalog target `fabric-26.1.2` | 3 GB | 2 GB |
| Valheim | `valheim` | Takaro BepInEx server plugin (WebSocket) | 4 GB | 4 GB |
| DayZ | `dayz` | `@TakaroIntegration` Enforce mod → loopback HTTP → Takaro TypeScript sidecar (WebSocket) | 6 GB | 4 GB² |
| RuneScape: Dragonwilds | `dragonwilds` | `libtakaro-dragonwilds.so` `LD_PRELOAD` plugin → loopback HTTP → Takaro TypeScript sidecar (WebSocket) | 4 GB | 8 GB³ |
| Rust | `rust` | `TakaroConnector.cs` Carbon plugin (WebSocket) | 8 GB | 12 GB |
| 7 Days to Die | `7d2d` | Takaro mod (WebSocket) | 8 GB | 32 GB¹ |
| Project Zomboid | `zomboid` | Takaro `-javaagent` inside the PZ server JVM (WebSocket) | 8 GB | 16 GB |
| Palworld | `palworld` | Third-party bridge → Palworld REST API | 12 GB | 10 GB |
| Conan Exiles | `conan-exiles` | Takaro TypeScript sidecar → Conan RCON | 12 GB | 35 GB |

³ `dragonwilds` runs as `takaro-dev-dragonwilds` with its data in `_data/dragonwilds-dev`.

² DayZ server files are **not** downloaded by the image: Steam app 223350 refuses
`login anonymous` (Bohemia T179224), so the Linux depot is fetched elsewhere with an
account that owns DayZ and dropped into `_data/dayz/server/`. Set `STEAM_USER` on the
`dayz` service only if you want the entrypoint to run SteamCMD itself.

¹ 7D2D downloads the game **twice**: once as the running server, and once as build
references for `games/7d2d/scripts/setup-environment.sh`, which needs the Managed DLLs to
compile the mod against. The second copy lands in `games/7d2d/_data/game-files`, outside
`dev-servers/`.

Total if everything is installed: **~100 GB of disk**. Running everything at once would need
~54 GB of RAM, which is why `start.sh` enforces a budget (see [Resource guards](#resource-guards)).

**Eco and Soulmask are not included** — this repo has no connector for either.

## One-time setup

```bash
cp dev-servers/.env.example dev-servers/.env
chmod 600 dev-servers/.env
$EDITOR dev-servers/.env        # set TAKARO_REGISTRATION_TOKEN, RCON_PASSWORD, ADMIN_PASSWORD
```

`dev-servers/.env` is gitignored and must never be committed.

You can start the downloads **before** you have a Takaro token — installing is mostly
downloading. Configs are rendered with a blank token and you fill them in afterwards with
one command, no reinstall and no re-download:

```bash
$EDITOR dev-servers/.env      # add TAKARO_REGISTRATION_TOKEN
just dev-reconfigure          # re-render every installed game's config
```

Then install the games. This is **sequential on purpose** — one game at a time, each
stopped before the next begins, so a 35 GB download never competes with a booting server:

```bash
just dev-install-all                       # everything, cheapest games first
just dev-install-all terraria valheim      # or just the ones you want
just dev-install terraria                  # a single game
```

Expect this to take hours on a cold box: Conan Exiles alone is ~35 GB and Rust builds a
~12 GB image. It is resumable — already-installed games are skipped, so you can interrupt
and re-run. Add `--force` to redo one.

## Daily use

```bash
just dev-status                       # what is installed, running, and what it costs
just dev-start minecraft-paper        # start one game (or several)
just dev-logs minecraft-paper -f      # follow its logs
just dev-stop minecraft-paper         # stop one game
just dev-stop                         # stop everything
just dev-validate                     # docker compose config + port-collision check
```

After editing connector code, rebuild and redeploy without a reinstall:

```bash
just dev-deploy minecraft-paper       # build via the repo's own scripts, copy artifact
just dev-stop minecraft-paper && just dev-start minecraft-paper
```

Rust is the exception — Carbon hot-reloads it, so `just dev-deploy rust` followed by
`c.reload TakaroConnector` over RCON is enough.

The scripts work without `just` too: `dev-servers/scripts/start.sh minecraft-paper`.

## Build toolchains

The connector builds need JDK 25 (Minecraft, Project Zomboid), Node (Conan sidecar) and the .NET SDK
(Valheim, Terraria). If the host has them, they are used directly. **If not, the builds
run in containers automatically** — no `apt install` and no sudo required:

| Connector | Host tool | Container fallback |
|---|---|---|
| Minecraft | `java` 25 | `eclipse-temurin:25-jdk` (whole multi-project build needs JDK 25 for Fabric Loom; Paper/NeoForge/core still target Java 21) |
| Project Zomboid | `java` 25 | `eclipse-temurin:25-jdk` (PZ B42 is class-file v69 / Java 25) |
| Conan Exiles | `npm` | `node:22-slim` |
| DayZ | `npm` | `node:22-slim` (sidecar is built by `docker compose build`) |
| Valheim | `dotnet`, `jq`, `zip`, `unzip` | `takaro-dev-valheim-builder` (built on first use from `images/valheim-builder/`) |
| Terraria | `dotnet` 9 | handled by `games/terraria/scripts/build-mod.sh` itself |
| Rust | none — Carbon compiles the `.cs` at runtime | — |

Build caches live in `_data/.toolcache/` so repeat builds are fast.

## Keeping deployed mods in sync with the repo

Deployed artifacts can silently fall behind the source after a `git pull` or a local
edit. `sync-connectors.sh` fingerprints each connector's **source tree** (SHA-256, with
`build/`, `bin/`, `obj/` and `node_modules/` excluded so build output never causes a
false positive) and compares it to what was recorded when the artifact was deployed.

```bash
just dev-sync-check          # report what is stale; exit 1 if anything is
just dev-sync                # rebuild + redeploy stale connectors
just dev-sync --restart      # ...and restart the ones that were running
just dev-sync --baseline     # record current source as deployed, without rebuilding
```

Every `deploy-connector.sh` run records a fingerprint automatically, so the baseline
stays correct without you thinking about it.

**Run it automatically on repo changes:**

```bash
just dev-install-hooks           # report-only after pull / merge / checkout / rebase
just dev-install-hooks --auto    # rebuild, redeploy and restart automatically
just dev-install-hooks --uninstall
```

The hooks are idempotent, are written into `.git/hooks` between clear markers so they
coexist with any hooks you already have, and never start or stop a game unless you chose
`--auto` — a `git pull` cannot disrupt a running test session.

**Palworld is handled differently.** Its bridge is a pinned third-party release rather
than repo source, so the check compares the pinned version in
`images/palworld-bridge/Dockerfile` against the latest GitHub release and tells you when
a newer one exists.

## Resource guards

The box cannot run every server at once, so:

- `start.sh` sums the estimated RAM of running plus requested games against
  `DEV_SERVERS_RAM_BUDGET_GB` (default **40**) and refuses to go over. Override per
  invocation with `--force`, or raise the number in `.env`.
- `start.sh --all` is refused outright unless you also pass `--force`.
- `install-all.sh` installs strictly one game at a time and stops each before the next.
- `install-all.sh` aborts if free disk falls below `DEV_SERVERS_MIN_FREE_GB` (default 40).

## Port map

Game ports listen on all interfaces. **Admin ports (RCON, REST, telnet, web dashboards)
bind to `127.0.0.1` only** — reach them over an SSH tunnel or a private VPN, e.g.
`ssh -L 25575:127.0.0.1:25575 <host>`.

| Game | Game ports (public) | Admin ports (localhost only) |
|---|---|---|
| Rust | 28015/udp | 28016 RCON (WebSocket) |
| Minecraft Paper | 25565/tcp | 25575 RCON |
| Minecraft NeoForge | 25566/tcp | 25576 RCON |
| Minecraft Fabric | 25567/tcp | 25577 RCON |
| Minecraft Fabric 26.1.2 | 25568/tcp | 25578 RCON |
| 7 Days to Die | 26900/tcp+udp, 26901-26902/udp | 8180-8182 (web dashboard, telnet) |
| Project Zomboid | 16261/udp, 16262/udp | 25582 RCON |
| Valheim | 2456-2457/udp | — |
| DayZ | 2302-2306/udp, 27116/udp (Steam query; 27016 inside the container) | 2310 BattlEye RCON, 8088 sidecar HTTP (inside the game netns only) |
| Terraria | 7777/tcp | 7878 TShock REST |
| RuneScape: Dragonwilds | 7797/udp | 18890 plugin HTTP, 18891 sidecar health — both inside the game netns only, never published |
| Conan Exiles | 7787/udp, 7788/udp, 27015/udp | 25580 RCON, 3010 sidecar HTTP |
| Palworld | 8211/udp, 27016/udp | 8212 REST, 25581 RCON, 3001 bridge HTTP |

Two deliberate shifts from stock defaults: 7D2D's web ports move from 8080-8082 to
8180-8182 (8080 collides with everything), and Conan moves off 7777/7778 so its row is
unambiguous next to Terraria's 7777/tcp.

`just dev-validate` fails if any two games ever publish the same host port and protocol.

## Takaro dashboard steps

Follow the official guide at
<https://docs.takaro.io/advanced/adding-support-for-a-new-game> for exact UI wording; the
shape is the same for every WebSocket connector here:

1. In the Takaro dashboard, add a game server using the **Generic / custom connector**
   type. Takaro gives you a **registration token**.
2. Put that token in `dev-servers/.env` as `TAKARO_REGISTRATION_TOKEN`. One token is
   enough for every game here.
3. Start a game. The connector dials out to `TAKARO_WS_URL`
   (`wss://connect.takaro.io/` by default) and identifies itself.
4. Identity is carried entirely by the **identity token** — there is no server-id field
   anywhere in these connectors. Each game has its own (`TAKARO_IDENTITY_*` in `.env`,
   e.g. `takaro-dev-paper`), so one Takaro organisation can hold all of them at once.
   Changing a game's identity token makes it register as a **brand-new** server.
5. Confirm the server shows as online in the dashboard, then install a module and run one
   of its commands in-game.

Two games do **not** follow that flow:

- **Terraria** — the plugin in this repo has no Takaro connection at all; it only writes
  `TAKARO_EVENT` markers into the TShock log and adds `/takaropos` / `/takarotp`. Takaro
  drives Terraria over the **TShock REST API**, so register it with host, REST port `7878`
  and a REST token. `install.sh` generates a token and prints it; set
  `TERRARIA_REST_TOKEN` in `.env` to keep it stable across reinstalls. The token also
  lands in `_data/terraria/tshock/config.json` under `ApplicationRestTokens`.
- **Palworld** — see below.

## Per-game notes

### Minecraft — two game versions side by side

Fabric runs a different Minecraft version from the other two:

| Service | Image | Minecraft version | Where it comes from |
|---|---|---|---|
| `minecraft-paper` | `itzg/minecraft-server:java21` | 1.21.11 | `MINECRAFT_VERSION` in `dev-servers/.env` |
| `minecraft-neoforge` | `itzg/minecraft-server:java21` | 1.21.11 | `MINECRAFT_VERSION` in `dev-servers/.env` |
| `minecraft-fabric` | pinned by the catalog target | pinned by the catalog target | `catalog/minecraft/targets/` |
| `minecraft-fabric-26.1.2` | pinned by the catalog target | 26.1.2 (pinned by the catalog target) | `catalog/minecraft/targets/fabric-26.1.2.json` |

A client can only join a server on its own version: 26.2 clients for Fabric, 1.21.11 clients for
Paper and NeoForge.

#### Fabric is driven by a catalog target

The Fabric rig does not choose a version at all. `install.sh` resolves the catalog target and
writes its values to `_data/.targets/minecraft-fabric.env`, which every `docker compose` call for
this file reads — image (by digest), game version, loader and launcher. The server jar, the
launcher jar and the Fabric API jar are downloaded by hash and recorded in
`_data/minecraft/fabric/.takaro/installed-target.json`.

```bash
just maint targets list --game minecraft       # which target the rig will use
dev-servers/scripts/install.sh minecraft-fabric
```

`start.sh` refuses to start the service when the data directory does not hold that target (or its
files changed on disk), and tells you to re-run the install.

**Migrating an existing rig data directory:**

```bash
dev-servers/scripts/install.sh minecraft-fabric --force
```

The data directory is kept, and the world, `server.properties`, `ops.json` and `config/` are
preserved — the install only replaces the files the target pins.

Fabric has **no hot reload** — restart the container after a deploy:

```bash
dev-servers/scripts/deploy-connector.sh minecraft-fabric   # builds in eclipse-temurin:25-jdk
just dev-stop minecraft-fabric && just dev-start minecraft-fabric
```

RCON (password comes from `RCON_PASSWORD` in `dev-servers/.env`):

```bash
docker exec takaro-dev-minecraft-fabric rcon-cli list
```

Server data lives in `dev-servers/_data/minecraft/fabric/`; connector config is
`config/takaro.json` there (environment variables override it) and logs are
`logs/latest.log` (raw WebSocket frames when `TAKARO_DEBUG=true`).

### Valheim
`companionMode` defaults to `optional` (set via `VALHEIM_COMPANION_MODE`), so vanilla
clients can join. The shipped production default is `required`, which needs
`takaro-valheim-companion.zip` installed into **every player's Valheim client**. Set it to
`required` in `.env` and reinstall to test that path.

The compile-reference cache (`games/valheim/_data/server`) and the runnable server
(`dev-servers/_data/valheim`) are deliberately separate directories —
`games/valheim/scripts/setup-environment.sh` refuses to write into a live server install. Never
point one at the other.

### Palworld — REST only
Palworld's connector is [mad-001/Palworld-Bridge](https://github.com/mad-001/Palworld-Bridge),
a **third party project not maintained by this repo**. The image pins release `v1.7.6` and
verifies its SHA256 before use.

The bridge has two layers, and only one works here:

- **Works:** the Palworld REST API surface — info, players, settings, metrics, announce,
  save, shutdown, stop, kick, ban, unban.
- **Does not work:** the `TakaroChat` UE4SS Lua mod, which provides in-game chat capture,
  teleport, inventory and item handling. UE4SS targets Windows, and this stack runs the
  native Linux server binary. Port 3001 is exposed for it, but nothing will connect.

Use a Windows host if you need Palworld chat events.

### Conan Exiles
The heaviest install (~35 GB) and the least certain: it runs the native Linux
`ConanSandboxServer.sh` launcher documented in `games/conan-exiles/README.md`, which is not
widely containerised. If SteamCMD does not produce that launcher, the entrypoint fails
loudly with an explanatory message.

`RconMaxKarma=1000` is set because Conan throttles repeated RCON and the sidecar polls.
`databasePath` and `itemCatalogPath` are left blank in the generated config — the save DB
only exists after a world has generated, and the item catalog needs a DevKit export. Fill
them in `_data/conan-exiles/bridge/TakaroConfig.txt` to enable the richer read paths.

### 7 Days to Die
The mod is XML-configured only; `install.sh` pre-renders
`_data/7d2d/ServerFiles/Takaro/Config.xml` before first boot so you never have to do the
start/stop/edit/restart dance. `games/7d2d/scripts/setup-environment.sh` asserts an exact
`Assembly-CSharp.dll` checksum for the live-proven game build — if the server image drifts
past it, the build fails loudly rather than producing a silently broken mod.

## Layout

```
dev-servers/
├── compose/      one file per game; each has its own compose project name
├── images/       Dockerfiles for the Conan server and the Palworld bridge
├── templates/    config templates — ${VAR} placeholders only, never a real value
├── lib/          common.sh: the single game registry every script reads
├── scripts/      install-all, install, deploy-connector, start, stop, status, logs, validate
└── _data/        gitignored: game data, world saves, and rendered runtime configs
```

Every rendered config that contains a token — `Config.xml`,
`com.takaro.valheim.cfg`, both `TakaroConfig.txt` files, TShock's `config.json` — is
written **only** under `_data/`, mode 0600.

This folder does not modify any connector: `games/rust/`, `games/minecraft/` and `games/7d2d/` keep their own
`docker-compose.yml` for connector development, with their own `_data/` and ports.

## Troubleshooting

**`missing dev-servers/.env`** — run the one-time setup above.

**`TAKARO_REGISTRATION_TOKEN is empty`** — install and start refuse to run without it, so
you never end up with a server that silently fails to register.

**A game will not start** — `just dev-logs <game>` shows the container output.
`just dev-status` shows whether it was ever installed.

**Terraria: no Takaro line in `dev-logs`** — expected. TShock writes plugin output to its
own log file, not container stdout. Confirm the plugin loaded with:
`grep -i takaro dev-servers/_data/terraria/tshock/logs/*.log`
(look for `Takaro Terraria Events plugin loaded`).

**Port already in use** — the connector-level environments (`games/rust/`, `games/minecraft/`,
`games/7d2d/`) publish overlapping ports. Stop those (`just rust-down`, `just minecraft-down`,
`just sevend2d-down`) before starting the dev-servers equivalents.

**Root-owned files in `_data/`** — some game images run as root. The scripts hand
ownership back automatically after each first boot; to do it by hand:
`docker run --rm -v "$PWD/dev-servers/_data:/t" alpine chown -R "$(id -u):$(id -g)" /t`

**Reset one game completely** — `just dev-stop <game> --down`, delete its directory under
`dev-servers/_data/` and its marker in `dev-servers/_data/.markers/`, then reinstall.
