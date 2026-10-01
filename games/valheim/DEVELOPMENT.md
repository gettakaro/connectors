# Takaro Valheim Connector — Development

Everything here is for people building, testing or changing the connector. Operators only
need [README.md](README.md).

## The catalog target

Everything here is built against one catalog target,
[`catalog/valheim/targets/linux-1.0.16.json`](../../catalog/valheim/targets/linux-1.0.16.json).
Unlike the other games in this catalog, a Valheim target pins **two** inputs:

- `inputs.server` — the Steam app, branch, build id and depot manifest that identify the
  dedicated server, plus the sha256 of each file the build and the run depend on.
- `inputs.bepinex` — the Thunderstore package (`denikson/BepInExPack_Valheim`) by
  namespace, name, version, zip sha256 and size. Thunderstore publishes no digest of its
  own, so the hash is self-recorded: it is written from two independent downloads that
  agreed.

An exact install is the depot set **plus** the pack unpacked into it, and both halves are
recorded in the install ledger. A replaced `BepInEx/core/BepInEx.dll` is as much a drifted
install as a replaced `valheim_server.x86_64`, and `takaro-maint ledger check` says so.

No script in this directory hard-codes a game build, a BepInEx version, an image digest, a
dependency URL or an artifact name. `scripts/lib-target.sh` resolves the target through
`takaro-maint targets resolve --game valheim` and exports it as `VALHEIM_*`:

| Key | What |
|---|---|
| `VALHEIM_TARGET`, `VALHEIM_REVISION` | the target id and the game version it names |
| `VALHEIM_FINGERPRINT`, `VALHEIM_FP16` | the hash of the whole record, and its short form |
| `VALHEIM_IMAGE`, `VALHEIM_TOOLCHAIN` | the server image and the .NET SDK image, both by digest |
| `VALHEIM_STEAM_APP/_BRANCH/_BUILDID/_DEPOTS` | the Steam pin |
| `VALHEIM_BEPINEX_PACKAGE/_PACK_VERSION/_URL/_SHA256/_SIZE/_FILE` | the pack pin |
| `VALHEIM_REFERENCES_DIR`, `VALHEIM_BEPINEX_DIR`, `VALHEIM_DEP_*` | where the build's inputs land, and where they come from |
| `VALHEIM_ARTIFACT_SERVER_PLUGIN` | the server plugin artifact name, with `{version}` still to substitute |

What lands where, for the fingerprint of the target you resolved:

| Path | What |
|---|---|
| `_data/references/<fp16>/` | the game's compile references and `.takaro/references.json` |
| `_data/deps/bepinex/<fp16>/` | the unpacked BepInEx pack and `.takaro/loader-version` |
| `_data/dist/<fp16>/` | the server plugin zip and its `.meta.json` sidecar |

A different target has a different fingerprint and therefore its own directories; nothing
is shared between builds for different server versions.

There is no SteamCMD anywhere in this directory. `takaro-maint steam references` downloads
only the assemblies the build needs, straight from the pinned depot manifest, and the pack
is fetched from its exact Thunderstore version URL — never from the `latest` alias. See
[maintenance/docs/steam-install.md](../../maintenance/docs/steam-install.md).

`setup-environment.sh` runs on whatever host invokes it — a laptop, a CI runner, the
maintenance image — so it asks for very little: `curl`, `file`, and the `python3`
`takaro-maint` already needs. The pack is unpacked by a reader that refuses an entry
escaping the staging directory, not by `unzip`.

## Quick Start

Run the reference-free build and tests (no game assemblies needed):

```bash
dotnet test mod/Takaro.Valheim.sln
```

Fetch the target's inputs and build the release archive, in the pinned .NET SDK image:

```bash
./scripts/setup-environment.sh --target linux-1.0.16
./scripts/build-release.sh <version> dist --target linux-1.0.16
```

`--target` may be left out; the game's default target is resolved instead.
`VALHEIM_BUILD_TOOLCHAIN=host` keeps the build in place rather than re-execing into the
SDK image, and then needs `dotnet`, `zip`, `unzip`, `jq`, `rg` and `file` locally.

Through the maintenance CLI the same build is `takaro-maint build --game valheim
--toolchain container`. Pass that flag: only `VALHEIM_BUILD_TOOLCHAIN` selects where this
game compiles, while `--toolchain` is what `build-manifest.json` records as
`toolchain.mode`, and its default (`host`) would make the manifest name a build
environment this connector never uses.

Build the plugin by hand against references you already have:

```bash
dotnet build mod/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -f net472 \
  -p:EnableValheimPluginBuild=true \
  -p:BepInExReferencePath=/path/to/BepInEx/core \
  -p:ValheimReferencePath=/path/to/valheim_server_Data/Managed
```

## Re-pinning

Valheim patches often, and BepInExPack moves on its own schedule, and
`catalog/valheim/game.json` declares a watch block for each. They do not deliver the same
thing. The Steam watch is a `game` watch: `takaro-maint scan` files one maintenance issue
per build it sees. The Thunderstore watch is a `framework` watch, and a framework
observation is reconciled into the game issue of the game version it is for -- which
Thunderstore does not publish for BepInExPack. So a moved pack is recorded as a head
observation and nothing else: read it out of `takaro-maint scan` (or the readiness state)
and re-pin by hand with step 3 below.

1. `takaro-maint steam pin --game valheim` — what does Steam serve on `public` now?
   `changed` lists the depots that moved.
2. `takaro-maint steam pin --game valheim --target linux-<new> --metadata --buildid <new>
   --record-files valheim_server.x86_64 --record-files
   valheim_server_Data/Managed/assembly_valheim.dll ... --write` — re-record the hashes
   from the new manifest into a new target record.
3. For a new pack: download the exact version zip twice, confirm the two sha256 agree, and
   write `inputs.bepinex.{version,sha256,size}`. Then
   `takaro-maint catalog validate --online`, which downloads it once more and compares.
4. `./scripts/setup-environment.sh --target linux-<new>` and
   `dotnet test mod/Takaro.Valheim.sln` — both bash behaviour harnesses run from there.
5. Re-prove the behaviour that matters (`takaro-maint verify --game valheim`), update
   `README.md`'s table with `takaro-maint docs render --game valheim --write`, and open a
   PR.

The target id, its fingerprint and the artifact name change with the build, which is the
point: nothing claims to have been proven on bytes it was not proven on.

## Windows references

The record's `support.notes` carries the Windows depot (`896662`) and the manifest of the
same build, for recovery only. `takaro-maint steam references` serves the pinned **Linux**
depot and nothing else: there is no platform loop and no Windows fallback, because a
fallback is how a build silently compiles against assemblies nobody pinned. If you need
Windows references — say the Linux depot is unavailable — download them by hand with
DepotDownloader against the recorded manifest and point
`-p:ValheimReferencePath=` at them; the resulting artifact is not a release artifact.

## `manifest.json` fields

The plugin zip carries a `manifest.json`. Three version-shaped fields live there and they
mean three different things:

| Field | What it is |
|---|---|
| `productVersion` | the connector release version, full SemVer (`3.0.3`, `3.0.3-dev.abc1234`) |
| `pluginVersion` | the numeric core BepInEx parses out of `[BepInPlugin]` (`3.0.3`) — the **plugin's** version |
| `bepInExPack.version` | the pinned Thunderstore pack version (`5.4.2351`) |
| `bepInExVersion` | the **loader assembly** version read out of the pack's `BepInEx.dll` (`5.4.23.5`) |

`bepInExVersion` used to carry the connector's own numeric core, which presented the
connector's version as BepInEx's. It now carries the real loader version, recorded by
`scripts/bepinex-loader-version.proj` into `_data/deps/bepinex/<fp16>/.takaro/loader-version`
at setup time. `target.{game,id,revision,fingerprint}` names the build the zip was made
for, and `processRole` is always `dedicated-server`.

## Catalogue names

`listItems` and `listEntities` return `code = prefab.name` (`SwordBronze`) and `name` = the
English display name (`Crude Bow`, `Raspberries`). `ValheimLocalizer` resolves
the `$token` through the game's own `Localization` singleton, which lives in
`assembly_guiutils.dll`; it is found by reflection, so the build needs no extra pinned
reference. Results are cached, and a token without a translation falls back to the code. The
same names are used for `giveItem` resolution and for the entity and weapon fields of events.

## Verification evidence policy

`takaro-maint verify --game valheim` boots a **dedicated server** with no game client attached.
What a run covers is startup, identify, the catalogue, an allowlisted console action, reconnect
after a dropped socket, and a clean stop. Chat, death, kill and item-delivery rows need a real
game client and are claimed only from a recorded run with one attached. When such a row moves,
say which run moved it and on which artifact — "the harness passed" is not an answer, because
the harness does not look.

## Architecture

Valheim has no RCON and no remote admin API, so the connector is a BepInEx plugin that
runs **inside the dedicated server process** and dials out to `wss://connect.takaro.io/`.
Players use plain vanilla Valheim; nothing is installed on clients.

- `mod/src/Takaro.Valheim.Core` — the game-independent protocol, configuration, models,
  policies and request dispatcher.
- `mod/src/Takaro.Valheim.Plugin` — the dedicated-server BepInEx adapter
  (`com.takaro.valheim`, plugin name `Takaro Valheim`).
- `tests/Takaro.Valheim.Core.Tests` — protocol, behavior, packaging and capability-registry
  tests.
- `capabilities.json` — the machine-readable support registry.

The plugin disables itself before Harmony patching or connector startup when it is not
running in a dedicated-server process; the server plugin still refuses graphical-client
processes. Never copy `TakaroValheim.dll` into a game client.

### Server-side chat relay

The server adds one `Takaro` entry to the player list it sends to clients. The entry exists
only in the outgoing packet; the server's own player list and history never contain it.
Vanilla clients send each chat line once per player-list entry and only render chat from
senders on that list, so this entry makes:

- player chat reach the server even when a player is alone on it, and
- server messages render as normal chat with the sender name.

Players therefore see a `Takaro` entry in the in-game player list. The startup log line
`Takaro Valheim chat participant 'Takaro' active (server-side chat relay).` confirms the
relay is running.

### Event sources and trust

- `chat-message` and `player-death` come from routed `Say`, `ChatMessage` and `OnDeath`
  RPCs. A packet is accepted only when its claimed sender equals the authenticated peer it
  arrived on and, for `Say` and `OnDeath`, it targets that peer's own character. Spoofed
  senders are rejected (unit-tested). The copies of one chat line a client sends (one per
  player-list entry) are collapsed within 3 seconds.
- `entity-killed` comes from the creature's network object when its owning game destroys
  it. The creature carries `Attackers<playerName>` marks and a kill-style modifier. The
  killer is the destroying player if it hit the creature, else the only player that hit it;
  with several hitters and none of them the destroyer, the kill is not guessed. A creature
  killed by a single blow carries no mark on the server, because mark and destroy happen in
  the same frame; that kill is credited only if the creature's ragdoll appears from the
  same game within 8 seconds, that player started a weapon attack (its animation trigger
  passes through the server) within 4 seconds before, and stood within 8 m (50 m with a bow,
  crossbow or staff). The weapon is the
  killer's equipped right or left item display name (the bow for ranged), or `Unarmed`.
- Game events are queued until Takaro accepts identify and flushed afterwards, so events
  raised while identifying are not lost.

### Main-thread constraint

Hook bodies stay on the Unity game thread only long enough to read the packet; sending and
serialisation run on pool threads. Valheim adapter calls are marshalled to the main thread
through a bounded `Update()`-drained action scheduler (`QueuedMainThreadActionScheduler`).

## Configuration

The plugin reads these BepInEx settings from the `[Takaro]` section of
`BepInEx/config/com.takaro.valheim.cfg`:

- `registrationToken`
- `serverName` (default `Valheim Server`)
- `identityToken` (written by the plugin after registration)
- `takaroWsUrl` (default `wss://connect.takaro.io/`)
- `logLevel` (default `Information`)
- `enableLogEvents` (default `true`)
- `commandAllowlistExact` (default `help`, semicolon-separated)
- `commandAllowlistPrefixes` (default empty, semicolon-separated)
- `chatSenderName` (default `Takaro`, at most 128 characters) — the name shown in game chat
  for Takaro messages unless Takaro sends its own sender name

The `companionMode` key from 3.x is ignored.

Never commit registration or identity tokens.

## Capability registry

`capabilities.json` uses three statuses, independently of its ownership/source metadata:

- `live-supported` — the path has valid historical live evidence.
- `schema-fallback` — the connector action/schema is available or live-proven, but Takaro's
  standard route cannot yet expose it end to end.
- `unsupported` — the path is unavailable or still lacks exact live proof.

Ownership values are `server-owned`, `upstream-blocked` or `unsupported`.

### Actions

| Action | Status | Source and behavior |
| --- | --- | --- |
| `testReachability` | `live-supported` | Reports connector reachability. |
| `getPlayers` | `live-supported` | Reads the Valheim dedicated-server player list. The `Takaro` chat entry is never included. |
| `getPlayer` | `unsupported` | Filtering exists, but Takaro exposes no single-player route to prove the final response shape. |
| `getPlayerLocation` | `live-supported` | Uses only a real peer/public position or a fresh 30-second server-observed last-known position; an unavailable lookup is rejected through a schema-valid payload error. |
| `getPlayerInventory` | `unsupported` | Valheim keeps inventories in the client profile; the server only sees equipped visuals. Returns an error instead of a fabricated `[]`. |
| `giveItem` | `live-supported` | Drops the items at the player's server-known position; the vanilla client's auto-pickup puts them in the bag (2026-10-01: 5 Raspberries and a Club landed in the inventory). |
| `sendMessage` | `live-supported` | Sent as normal chat from the `Takaro` player-list entry, globally or to one recipient. The trimmed `opts.senderNameOverride` is used for that message; a missing or blank value displays as `chatSenderName` (default `Takaro`). |
| `executeConsoleCommand` | `live-supported` | Runs only exact or prefix-allowlisted commands. |
| `listItems` | `live-supported` | Lists item prefabs visible to the server, with English display names. |
| `listEntities` | `live-supported` | Lists non-player character prefabs visible to the server, with English display names. |
| `listLocations` | `schema-fallback` | The official raw Generic Connector action/schema live-returned 11,293 nested `ILocationDTO` objects, but the standard Takaro route at `0c63cf1c` throws `NotImplementedError` before requesting them. |
| `getMapInfo` | `unsupported` | Returns an immediate schema-valid payload error; the dedicated server does not expose client map metadata. |
| `getMapTile` | `unsupported` | Returns an immediate payload error; the dedicated server does not expose rendered client map tiles, and Takaro's API does not support map tiles for Generic-connector servers. |
| `teleportPlayer` | `live-supported` | Routes Valheim's built-in `RPC_TeleportTo` to the server-known character ZDO, and returns `character_unavailable` when that identity is missing. |
| `kickPlayer` | `live-supported` | Sends Valheim's built-in `Kicked` RPC and logs the supplied reason. It never calls `ZNet.Disconnect(peer)` directly, which is what previously crashed the headless server. |
| `banPlayer` | `live-supported` | Writes the player identifier into Valheim's official ban list and disconnects them with the built-in `Kicked` RPC. **The ban reason is discarded**: Valheim's ban list stores only one identifier per line, so a reason supplied by Takaro is read back as `""`. |
| `unbanPlayer` | `live-supported` | Removes the identifier from Valheim's official ban list; `listBans` then returns an empty array. |
| `listBans` | `live-supported` | Reads Valheim's official ban entries. |
| `shutdown` | `live-supported` | Writes its success response before quitting, then shuts down on Unity's main thread. Valheim performs a clean `ZNet` shutdown and the server process exits. |

### Events

| Event | Status | Source and behavior |
| --- | --- | --- |
| `log` | `live-supported` | Emits connector log events. |
| `player-connected` | `live-supported` | Derived from dedicated-server player snapshots after a real server position is observed. |
| `player-disconnected` | `live-supported` | Derived from the same snapshot tracker. |
| `chat-message` | `live-supported` | Routed `Say`/`ChatMessage` RPCs from the authenticated peer, collapsed within 3 seconds; works when a player is alone. |
| `player-death` | `live-supported` | Routed `OnDeath` RPC targeting the authenticated peer's own character, with player, position and timestamp. |
| `entity-killed` | `live-supported` | Creature destroy with `Attackers` marks; coverage is partial for single-blow kills (see *Event sources and trust*). |

## Server-owned action semantics

`giveItem` creates world drops at the player's server-known position, and the vanilla
client's auto-pickup collects them into the bag. A world drop is not a private mutation:
another player standing on the same spot could pick it up first. **This applies to shop
deliveries too**, since a shop claim delivers through `giveItem`. Takaro returns the empty
success payload, so the API cannot tell whether the player has picked the items up yet.

The adapter accepts at most 1,000 items and 100 world-drop stacks per request, validates
quality, resolves prefab codes or display/name tokens, splits oversized stacks, and returns
an error when no server-owned player position is known.

Player location never returns a fabricated origin. A live peer/public observation is cached
for 30 seconds so Takaro can enrich a disconnect with the player's real last-known
position; the cache is player-keyed, expires, and clears when Valheim replaces its
network/world instance. `player-connected` emission waits until such a real observation
exists. If no current or fresh observation exists, the connector sends the position DTO's
required numeric fields plus `payload.error`. At Takaro source commit `0c63cf1c`, the app
connector validates that payload and `Generic.requestFromServer` rejects `payload.error`
before returning a position. Root-level response metadata is not used by that consumer.

Remote inventory is unavailable on the dedicated server, so `getPlayerInventory` never
becomes a fabricated empty array.

Other failure-capable actions return immediately. At Takaro source commit `0c63cf1c`,
validation-free actions such as `giveItem`, messaging, teleport, moderation and shutdown
accept `{ error: "code: message" }`, which `Generic.requestFromServer` rejects without
waiting for a timeout. Validated object actions add only their required DTO fields before
the same payload error; `testReachability` instead returns `connectable:false` with an
actionable reason because that route bypasses the Generic error check. Array-validated
actions cannot carry a top-level JSON error; their ordinary server-owned paths return
arrays, and any actual failed array path is suppressed rather than fabricating an empty
result.

The connector distinguishes a confirmed empty collection from an unavailable Valheim
runtime source. `getPlayers`, `listItems`, `listEntities`, `listLocations` and `listBans`
return `[]` only when their required server singleton and collection exist. During world
startup or reload, `runtime_unavailable` is suppressed for these array DTOs and lifecycle
polling preserves its prior snapshot instead of fabricating an empty server or a false
disconnect. A missing `getPlayer` match returns an immediate `player_not_found` payload
error.

## Evidence boundary

Historical dedicated-server evidence from 2026-06-21/22 and the 2026-07-10 turn runs covers
vanilla-client player location, built-in teleport, moderation, delayed shutdown, player
connect/disconnect persistence, and an official raw `listLocations` response containing
11,293 nested locations; the standard Takaro `listLocations` route remained unavailable, so
that action is `schema-fallback`, not `live-supported`.

On 2026-09-02 the deployed `2.0.1` artifact was run against the reusable Takaro connector
acceptance checklist with a real game client attached. That run moved `kickPlayer`,
`banPlayer`, `unbanPlayer` and `shutdown` from `unsupported` to `live-supported`, re-proved
the module command loop and the shop purchase path end to end, and found that `banPlayer`
discards the ban reason.

The 2026-10-01 server-only run (Valheim 1.0.16, vanilla client) proved `chat-message`,
`sendMessage` (global, whisper and `opts.senderNameOverride`, rendered in normal chat),
`player-death`, `giveItem` with auto-pickup, `teleportPlayer`, catalogue display names and
the identify-gated event queue. Ledgers and handoffs for these runs are kept in the private
workspace repo.

## Release build

CI builds and publishes through `.github/workflows/valheim.yml`, which hands off to the
shared `connector-release.yml`: one build job per target, a second build of the same commit
compared byte for byte, then aggregate publication with `SHA256SUMS`, the legacy asset
alias and a compatibility record naming the exact inputs.

Locally, from `games/valheim/`:

```bash
./scripts/setup-environment.sh --target linux-1.0.16
./scripts/build-release.sh 0.1.0 dist --target linux-1.0.16
```

`setup-environment.sh` writes game compile references only to
`VALHEIM_REFERENCE_CACHE_DIR`, which defaults to `_data/references/<fp16>`. A valid Managed
directory can be reused read-only from any configured location. An invalid non-empty
directory is writable only when it carries the setup script's completed ownership marker;
otherwise setup refuses and directs the caller to a separate cache. The legacy
`VALHEIM_SERVER_DIR` variable remains a safe fallback for read-only valid references or
explicitly owned/empty caches, but it must not point setup at a live dedicated-server
installation. BepInEx comes from the pinned Thunderstore
`denikson/BepInExPack_Valheim` version, verified by sha256 and by the version its own
`manifest.json` declares, and published atomically into `_data/deps/bepinex/<fp16>`: a
failed download or a hash mismatch leaves the previous pack exactly as it was.

The release produces one zip per target, named after the target
(`takaro-valheim-plugin-linux-1.0.16-<version>.zip`), with a `.meta.json` sidecar recording
the target, the fingerprint and the role (`server-plugin`). The old unsuffixed name
`takaro-valheim-plugin.zip` is published alongside as a byte-identical alias of the default
target. The zip contains the dedicated-server plugin, Core and required runtime
dependencies, and excludes host-provided game, Unity, BepInEx, Harmony, Jotunn, debug and
host files. There is no client-side package.

## Versioning

The release version argument must be valid SemVer with major, minor and patch values no
greater than 65534. The exact full SemVer remains in assembly informational/package
metadata, the packaged README and `manifest.json`. BepInEx 5 parses its loader-facing
attribute with `System.Version`, so that one value is deliberately normalized to numeric
`major.minor.patch`; stable versions such as `1.0.0` therefore match exactly, while a
version such as `1.0.0-rc.1+build.2` loads as `1.0.0`. Numeric assembly/file metadata uses
`major.minor.patch.0`. The build generates both compile-time values under the intermediate
output directory and does not edit tracked source files.

Environment setup requires the host `file` utility to identify every required Valheim and
BepInEx DLL as a real `PE32 ... Mono/.Net assembly`. Compile references and the BepInEx
pack are each built in a sibling staging directory, validated there, marked as an owned
cache only after validation, and published by directory rename with rollback, so a failed
or interrupted run leaves whatever was there before byte-identical and never injects or
replaces files inside an unowned live server tree.
