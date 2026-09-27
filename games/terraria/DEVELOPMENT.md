# Terraria connector — development

Developer, architecture and build notes for the Terraria connector. Operator install steps live
in [README.md](README.md).

## Architecture

Terraria support is made of two parts that run on the Terraria dedicated server host:

- `mod/TakaroTerrariaEvents` — a server-side TShock plugin that emits structured `TAKARO_EVENT`
  markers for events TShock REST does not expose, and registers admin-only helper commands for
  coordinate-based location, inventory, give, teleport, ban and unban.
- `bridge/` — a TypeScript sidecar that connects outbound to Takaro over WebSocket and maps
  Takaro actions onto the TShock REST API.

TShock runs inside the Terraria dedicated server and exposes a REST API. The bridge stays outside
the game process, calls that REST API, and connects outbound to Takaro, so no inbound port has to
be opened for Takaro.

```text
Takaro  <--websocket--  bridge/  --REST-->  TShock  --hooks-->  plugin
                             ^                                     |
                             +--------- tails TShock log ----------+
```

## The catalog target

Nothing here names a TShock build, an image digest, a reference hash or an artifact name. One
record does — `catalog/terraria/targets/tshock-v6.2.1.json` — and every script, the dev rig and CI
read it through `takaro-maint targets resolve`, so they all resolve the same bytes.

```bash
maintenance/bin/takaro-maint targets resolve --game terraria --format env --prefix TERRARIA
```

What the record pins, and why each is a separate thing:

| | |
| --- | --- |
| `runtime.container` | `ghcr.io/pryaxis/tshock` by **tag and digest**. These are the bytes that run. A floating tag (`stable`, `latest`, `6`, `6.1`) fails `catalog validate`. |
| `build.references` + `build.deps` | the three assemblies the plugin compiles against — `TShockAPI.dll`, `OTAPI.dll`, `TerrariaServer.dll` — taken **out of that image**, by sha256. `takaro-maint verify` re-hashes them inside the booted container, so "compiled against" and "running against" are one claim. |
| `inputs.server` | the upstream TShock release zip of the same tag and source revision. It is the **version identity**: the thing `catalog validate --online` re-verifies over HTTP and the install ledger records. Its `OTAPI.dll` is byte-identical to the image's; its other two are a separate compile of the same source, which is why the compile references come from the image. |
| `build.toolchain`, `build.deps["bridge-runtime"]` | the .NET SDK and Node images, by digest. `Dockerfile.builder`'s `FROM` must equal the toolchain reference — a test compares them. |

Moving to a new TShock build is a new target file, never an edit to this one:

1. Add `catalog/terraria/targets/tshock-v<x.y.z>.json` with the new tag, digest and reference
   hashes (record each hash twice from two independent extractions and require them to agree).
2. `takaro-maint catalog validate` and `catalog validate --online`.
3. `games/terraria/scripts/setup-environment.sh --target tshock-v<x.y.z>` — the references land
   in their own `_data/refs/<fp16>/`, so the old target's cache is untouched.
4. `takaro-maint build --game terraria --target tshock-v<x.y.z>` and `takaro-maint verify`.
5. Flip `default` when the new target is proven; retire the old one when nothing ships it.

## Plugin

### Build

Extract the reference assemblies for the target once:

```bash
games/terraria/scripts/setup-environment.sh [--target tshock-v6.2.1]
```

They come out of the pinned image **by digest** (`docker create`, `docker cp`) into
`_data/refs/<fp16>/`, and every file is checked against the sha256 the catalog pins. There is no
override and no fallback: a stale cache for another target exits 7, an altered assembly exits 5,
and a digest the registry will not serve exits 4.

Build the plugin:

```bash
games/terraria/scripts/build-mod.sh [--target tshock-v6.2.1]
```

The compile runs inside the pinned .NET SDK image — never the host's `dotnet` — with
`-p:Deterministic=true -p:ContinuousIntegrationBuild=true -p:DebugType=none
-p:IncludeSourceRevisionInInformationalVersion=false`, so two builds of one commit produce
identical bytes. The last flag is what makes that true off this machine: without it the SDK reads
the mounted tree's git HEAD and appends `+<sha>` to the assembly's informational version, so a CI
build (whose HEAD on a pull request is GitHub's throwaway merge commit) and a rebuild from a
worktree could never agree. The assembly now carries the version it was asked for and nothing
else. The output is:

```text
games/terraria/_data/build/TakaroTerrariaEvents/TakaroTerrariaEvents.dll
```

### Package

Both roles are built and packaged together, because a release carrying one of them is incomplete:

```bash
maintenance/bin/takaro-maint build --game terraria --version 0.2.2 --out dist
```

That runs `scripts/build-release.sh`, which resolves the target, runs the three build steps and
packages each role inside the builder image (`SOURCE_DATE_EPOCH` = the committer time of the
checked-out commit, so the zip entries carry no clock). A release build checks out the tag, so its
archives rebuild byte for byte from that tag. A pull-request preview artifact is built from the
merge commit GitHub synthesises, so its zip entries carry *that* commit's time: its DLL matches a
local rebuild, its zip envelope does not. Pass `SOURCE_DATE_EPOCH` yourself to pin it. It writes
`dist/takaro-terraria-plugin-tshock-v6.2.1-<version>.zip`,
`dist/takaro-terraria-bridge-tshock-v6.2.1-<version>.zip` and a `.meta.json` beside each, which is
how a zip carries its target identity (`takaro-maint artifact validate` reads it).

### Runtime commands

```text
/takaropos <player>
/takarotp <player> <x> <y>
/takaroinv <player>
/takarogive <player> <item> <amount>
/takaroban <player> <reason>
/takarounban <player>
```

These are server-side TShock commands intended for connector automation, and all of them require
the `takaro.admin` TShock permission.

A REST user in the `superadmin` group already satisfies this through TShock's wildcard permission,
which is the common setup and needs no extra configuration. For a more narrowly scoped user, grant
`takaro.admin` explicitly: without it `teleportPlayer` fails, `getPlayerLocation` reports `0,0,0`,
and `getPlayerInventory` returns an empty list, all rather than failing loudly.

`sendMessage` runs TShock's own `/broadcast`, so the message reaches the server console as well
as the players — that command needs `tshock.broadcast`. When the REST user does not hold it the
bridge falls back to `/v2/server/broadcast`, which reaches the players but logs nothing, so chat
delivery never depends on the permission.

## Bridge

### Build

```bash
cd games/terraria/bridge
npm ci
npm test
npm run build
```

`scripts/build-bridge.sh [--target ...]` does the same inside the target's pinned Node image and
then re-installs with `npm ci --omit=dev`, because the release archive ships its production
`node_modules` — lock-pinned, so a deploy is file-only and an operator installs nothing.

### Package

Use `takaro-maint build` (above). `scripts/build-bridge-release.sh <version> <out-dir>` is kept as
a thin wrapper that builds the whole set.

### Local endpoints

```text
GET /health     bridge, Takaro identification, and TShock reachability
GET /coverage   per-action and per-event support status
```

`/health` returns `{ ok, takaroIdentified, gameServerId, tshockReachable, lastPollAt }`.

### CI

`.github/workflows/terraria.yml` runs the bridge's own tests (`npm ci && npm test && npm run
build`) on every pull request **and on a release** — they are this connector's contract evidence —
and delegates the release itself to `connector-release.yml`, which builds every catalog target and
publishes one set with checksums, a compat record and the legacy aliases.

It passes `runtime: false`. A Terraria run is two containers — the pinned TShock image and a Node
sidecar that joins its network namespace — which the harness does on a rig; runtime CI for this
connector is deferred, so the release claims `contract` verification rather than `runtime`.

## Verification

`takaro-maint verify --game terraria` boots the pinned image and drives it through
`games/terraria` hooks in `maintenance/src/takaro_maint/games/terraria/`:

- `container_command` puts the world on the server's command line. TShock reads no environment
  variable for it, and without it the server stops on its interactive world-selection menu.
- the container runs as **root**, because this image gives no choice: TerrariaServerAPI opens
  its own `ServerLog.txt` in `/server` and TShock writes `/server/GeoIP.dat`, neither of which is
  a declared volume, so a `--user` run dies before it has read its configuration.
  `after_shutdown` hands the run's data directory back to the calling user afterwards.
- `before_boot` writes `tshock/config.json` (REST on, one application token) and the bridge's
  `TakaroConfig.txt`, both mode 0600. Neither value is ever printed.
- `after_boot` starts the bridge as a **second container** with `--network container:<server>`, so
  TShock's REST API is on the bridge's own `127.0.0.1:7878`. It can only be asked for once the
  server container exists, which is why it is a hook and not part of the run's own argv.

The checks it adds are `handshake`, `items`, `entities`, `action`, `references` and `reconnect`.
A Terraria report reaches **`startup`** and never claims `protocol`: `identify` and
`connector-load` look for lines in the *server* log that this connector does not write (it writes
them in its own log — `handshake` asserts both), `catalog-entities` would assert a registry
Terraria does not have, and `catalog-items` spot-checks a Minecraft id.

## Watching upstream

Two independent providers, because a TShock release and the image built from it are published by
different systems at different times:

- **game** — `github-release` on `Pryaxis/TShock`, channel `release` on `^v\d+\.\d+\.\d+$` with
  the linux-x64 asset. Every TShock release is a new revision: a TShock patch on an unchanged
  Terraria version still matters to this connector. The Terraria version is in the release name
  and the asset name; no provider reads a DLL.
- **framework** — `oci-registry` (generic; it names no game) on `ghcr.io/pryaxis/tshock`. It lists
  the repository's tags, fetches the manifest for each tag a channel selects, and records the
  digest it hashes to.

Limits this states rather than papers over:

- **Tags are mutable.** A tag is observed at the digest it resolves to *right now*, and the
  revision folds that digest in (`6.1.0.911459f0`). What was pushed over a tag between two scans
  was never seen, so the source's history is `heads-only` and the replaced digest is not claimed.
- **Floating tags are never observed.** `latest`, `stable`, `6` and `6.1` match no channel's tag
  pattern, so no manifest is ever fetched for them and nothing is pinned to one.
- **An image carries no game version.** Nothing in a manifest says which release it was built
  from, so the watch's channel declares `gameRevision: "v{tag}"` — the template naming the release
  the game watch observes. That is what joins the readiness row to the release issue; without it
  the row cannot be joined and is reported missing rather than guessed at.
- **Access is anonymous but not unauthenticated.** ghcr.io answers a public repository only after
  a bearer challenge: the first `GET` returns 401 with `WWW-Authenticate`, the realm is asked for
  a token, and the request is retried once with it. That token is registered as a secret and never
  reaches stdout, stderr, a fact or a log line.

## Coverage

Every Takaro action has one explicit outcome, registered in `bridge/src/takaro/coverage.ts`.
Status meanings:

- **Supported** — reachable through TShock REST, a raw command, polling or log tailing.
- **Not applicable** — the concept does not exist in Terraria, or TShock exposes no way to reach
  it. A Takaro-valid empty or disabled response is returned so callers do not break.
- **Not built yet** — technically reachable, but not implemented here. These are the real roadmap
  items.

These are implementation statuses, not proof that an action works on a live server; see the table
in [README.md](README.md) for what has actually been verified.

### Actions

| Action | Status | Notes |
| --- | --- | --- |
| `testReachability` | Supported | Token test plus `/v2/server/status`. |
| `getPlayers` | Supported | From TShock REST. |
| `getPlayer` | Supported | From TShock REST. |
| `getPlayerLocation` | Supported | Plugin command `/takaropos`. |
| `getPlayerInventory` | Supported | Plugin command `/takaroinv`. |
| `sendMessage` | Supported | `/v2/server/broadcast`. Global and per-recipient. |
| `executeConsoleCommand` | Supported | Allowlisted by exact match and prefix. |
| `giveItem` | Supported | `/give`, with name-to-item-code resolution. |
| `teleportPlayer` | Supported | Plugin command `/takarotp`. |
| `kickPlayer` | Supported | TShock console command. |
| `banPlayer` | Supported | Plugin command `/takaroban`, banning UUID and IP. |
| `unbanPlayer` | Supported | Plugin command `/takarounban`, clearing every identifier. |
| `listBans` | Supported | `/v2/bans/list`. |
| `listItems` | Supported | 6147-entry catalog built from the server assemblies. |
| `shutdown` | Supported | `/v2/server/off`, gated behind `enableShutdown`. |
| `listEntities` | Not applicable | Terraria NPCs spawn from world state; there is no queryable entity registry. |
| `listLocations` | Not applicable | Terraria has no named-location concept for Takaro to list. |
| `getMapInfo` | Not applicable | Returns a disabled map DTO. TShock exposes no map metadata. |
| `getMapTile` | Not applicable | TShock does not render map tiles. |

### Events

| Event | Status | Source |
| --- | --- | --- |
| `player-connected` | Supported | Derived from TShock player snapshots. |
| `player-disconnected` | Supported | Derived from TShock player snapshots. |
| `player-death` | Supported | Plugin `TAKARO_EVENT` marker, including the resolved `attacker`. |
| `entity-killed` | Supported | Plugin `TAKARO_EVENT` marker. |
| `chat-message` | Supported | Parsed from the TShock log (best effort). |
| `log` | Supported | Tailed from configured TShock log files. |

Set `logFiles` in the bridge config to the TShock log directory, otherwise the log-derived events
above are not delivered. TShock opens a new log file on every restart; pointing at the directory
lets the bridge follow that rotation, whereas a single file path goes silent after the first
restart.

The tailer never ships lines matching `logExcludePatterns` (default
`takaro-rest executed:,RestManager:`) as `log` events. TShock logs every REST call the bridge
itself makes, so without this the bridge feeds its own traffic back to Takaro and trips the rate
limiter, which silently drops real gameplay events. The filter applies only to plain `log`
passthrough, so a chat line or a death whose text happens to match is still delivered. Set an
empty value to disable filtering, or a comma-separated list to replace the defaults.

`entity-killed` reports a `weapon`, taken from the killer's held item at the moment the kill
fires. Terraria records no damage source on NPC death, so this is a proxy rather than exact
attribution: a projectile fired earlier can land after the player swaps weapons, and minion,
sentry, or damage-over-time kills may credit an item that dealt none of the damage. It reports
`unknown` when no killer or held item resolves.

## Items and inventory

Terraria has items, and both directions work. The bridge ships a static catalog of 6147 items
extracted from the server assemblies and resolves a display name such as `Wood` to the numeric
code TShock's `/give` expects.

`bridge/src/terraria/itemCatalog.ts` is generated, and its names are the internal ids split on
their capitals rather than Terraria's own display strings. That splitting is imperfect: about 120
names glue a short word onto the one before it (`A Horrible Nightfor Alchemy`, `Bandof
Regeneration`) and apostrophes are dropped throughout (`Aarons Helmet`). Resolution is unaffected,
because `resolveTerrariaItemCode` compares names with everything but letters and digits removed,
so `A Horrible Night for Alchemy` and `Aaron's Helmet` each still find their code; what is wrong
is the name Takaro shows. The fix is to regenerate the file from the pinned image's `OTAPI.dll`
manifest resource `Terraria.Localization.Content.en-US.Items.json` (`ItemName.<InternalName>` →
display name, ids from the `Terraria.ID.ItemID` constants), keeping the internal name as an alias.
That generator does not exist yet; this release ships the derived names knowingly.

Inventory reading is plugin-backed. `/takaroinv` reports every container a player owns, skips
empty slots, and aggregates duplicate item types across all of them into a single entry by summing
stacks:

| Container | Contents |
| --- | --- |
| `inventory` | main slots |
| `armor` | armor, accessories, and their vanity slots |
| `dye` | dye slots |
| `miscEquips` | pet, light pet, mount, and grapple slots |
| `miscDyes` | dyes for those misc slots |
| `trashItem` | trash slot |
| `bank`, `bank2`, `bank3`, `bank4` | Piggy Bank, Safe, Defender's Forge, Void Vault |
| `Loadouts` | stored equipment loadouts |

Loadouts do not double-count. `EquipmentLoadout.Swap` exchanges items with `player.armor` and
`player.dye` rather than copying them, so the active loadout's own arrays hold only empty items
while it is equipped and the empty-slot check drops them. Only the inactive loadouts contribute
entries.

Excluded are Terraria's transient engine arrays and its cached accessory-effect items, such as
`starCloakItem`, which are internal state rather than possessions and would otherwise be reported
as phantom items.

Takaro does not call `listItems` on demand. It runs a `syncItems` job when a game server is
registered, hourly thereafter, and on manual trigger, and that job checks reachability first. If
the connector is not attached at registration time the initial sync is skipped and Takaro's item
table for that server stays empty until the next successful sync. Attach the bridge before
registering the server, or trigger the job manually afterwards.

## Bans

Bans go through the plugin's `/takaroban`, not TShock REST. TShock matches an untyped name ban
only against players who authenticated under that name, so on a server where players join
unauthenticated a REST name ban records cleanly and the player reconnects straight through it.

The plugin bans `uuid:<TSPlayer.UUID>` plus `ip:<address>`, which TShock matches on every join,
and disconnects the player. The IP is banned alongside the UUID because a reinstalled client
produces a fresh UUID.

Because a banned player is offline — so their UUID can no longer be read from a live `TSPlayer` —
each ban is tagged `[takaro:<name>]` in its reason, and `/takarounban` searches the ban list by
that tag. Without that tag an unban can only ever succeed for a player who is not actually banned.

Without the plugin loaded, both actions fall back to the old REST name ban, so an operator running
the bridge alone keeps prior behaviour rather than losing bans outright.

## Safety

Raw console execution is allowlisted by exact command and by prefix. Shutdown is separately gated
behind `enableShutdown=true` and is off by default. Application REST tokens and Takaro
registration tokens are operational secrets: keep them in `TakaroConfig.txt` or the environment
(`TAKARO_REGISTRATION_TOKEN`, `TSHOCK_TOKEN`, `TSHOCK_USERNAME`, `TSHOCK_PASSWORD`), never in
version control.

## Version support

The plugin builds against the catalog target's pinned image, not against whatever a tag points at
today. TShock must match the Terraria server protocol version, and Terraria clients must match the
server. A Terraria client newer than the TShock build is rejected at join time with
`You are not using the same version as this server.`
