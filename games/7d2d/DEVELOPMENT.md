# Takaro 7D2D Mod — Development

Notes for working on the mod itself. Server operators only need
[`README.md`](README.md).

## The target

Everything here is built against one catalog target at a time. The default is
[`catalog/7d2d/targets/linux-3.3.0.b18.json`](../../catalog/7d2d/targets/linux-3.3.0.b18.json)
(V 3.3.0 b18); [`linux-3.2.0.b10.json`](../../catalog/7d2d/targets/linux-3.2.0.b10.json) is still
built. A target records the Steam app, branch, build id and depot manifest of its build, the sha256 of the
assemblies the mod compiles against, the Mono image the build runs in, the pinned third-party
dependencies and the server image the rig boots. No script here hard-codes any of it —
`scripts/lib-target.sh` resolves the target and exports `SEVEND2D_*` for the rest.

Each catalog target builds its own artifact from the same sources. Where the game's API changed
between builds, `sevend2d_api_defines` in `scripts/lib-target.sh` turns the target's revision into
a compile-time symbol (`SEVEND2D_V3_3` for V 3.3.0 and later), which `build-mod.sh` and
`test-contract.sh` pass to the compiler. V 3.3.0 keeps player inventories in serialised blobs and
replaced `ItemValue.Modifications` with accessors, so `StateMirror` and `GiveItemHandler` have one
branch per API generation.

There is no SteamCMD anywhere in this directory. `takaro-maint steam references` downloads only
the assemblies the build needs, straight from the pinned depot manifests. See
[maintenance/docs/steam-install.md](../../maintenance/docs/steam-install.md).

## Quick start

From the monorepo root:

```sh
just sevend2d-setup          # reference assemblies + dependencies for the default target
just sevend2d-build          # compile the mod
just sevend2d-build-deploy   # build and deploy into the dev rig
just sevend2d-test-contract  # the Generic Connector contract harness
```

Or from inside `games/7d2d/`:

```sh
./scripts/setup-environment.sh --target linux-3.3.0.b18
./scripts/build-mod.sh --target linux-3.3.0.b18
./scripts/build-release.sh <version> <out-dir> --target linux-3.3.0.b18
```

`--target` may be left out everywhere except `build-release.sh`, which insists on it: a release
artifact is named after the build it was made for, so it may not be built for "whatever the
default is today".

What lands where:

| Path | What |
|---|---|
| `_data/7dtd-binaries/<fingerprint>/` | The target's reference assemblies and the built dependencies, plus `.takaro/references.json` and `.takaro/deps.json`. |
| `_data/build/` | msbuild output (`Mods/Takaro`) and the staged folder the release zip is made from. |
| `_data/dist/<fingerprint>/` | `takaro-7d2d-mod-<target>-<version>.zip` and its `.meta.json`. |

A different target has a different fingerprint and therefore its own directories; nothing is
shared between builds of different server versions.

## The rig

`dev-servers/scripts/install.sh 7d2d` installs the exact pinned build into
`dev-servers/_data/7d2d/ServerFiles` through `takaro-maint install`, records it in
`ServerFiles/.takaro/installed-target.json` and renders `Takaro/Config.xml`. The compose file
pins the server image by digest and runs it with `START_MODE=1` — start what is installed. The
install writes `DONT_REMOVE.txt`, which is what stops the image's own LinuxGSM auto-install from
replacing the pinned build with the current branch head.

`dev-servers/scripts/deploy-connector.sh 7d2d` builds the target's artifact and deploys it by
manifest row, so the mod in `Mods/Takaro` is the one the ledger records.

## Moving to a new server build

1. `just sevend2d-pin` — what does Steam serve on `public` now? `changed` lists the depots that
   moved.
2. `takaro-maint steam pin --game 7d2d --buildid <new> --record-files <each declared file> --write`
   — re-record the hashes from the new manifest and write the new pin.
3. `./scripts/setup-environment.sh` and `./scripts/test-contract.sh` — the contract harness must
   pass against the new assemblies.
4. Re-prove the behaviour that matters on the rig, update the table in `README.md`, and open a PR.
   The target id, its fingerprint and every artifact name change with the build, which is the
   point: nothing claims to have been proven on bytes it was not proven on.

## Architecture

The mod mirrors game state into a LiteDB database (a file in the world's save
directory, see [Storage](#storage)) so that Takaro read requests never touch the
game simulation:

- Game events (join, disconnect, player data saves) and a ~3s position sampler
  update the mirror from the game main thread; DB writes happen on a dedicated
  writer thread.
- Read requests (`getPlayers`, `getPlayerLocation`, `listItems`, `listBans`, …)
  are answered from LiteDB on the WebSocket thread.
- Action requests (`giveItem`, `teleportPlayer`, `banPlayer`, …) are marshalled
  onto the game main thread via a dispatcher and awaited asynchronously.

It is server-side only and connects to Takaro through the Generic Connector
Protocol over an outbound WebSocket. See
[`IMPLEMENTATION_STATUS.md`](IMPLEMENTATION_STATUS.md) for the per-endpoint data
flow, the state-mirror diagram and the staleness bounds. Its "Protocol" section
describes what the identify handshake declares (protocol versions, Capability
Manifest, game identifier, Connector Migration support) and how events are
numbered and acknowledged. A new action must be added to
`RequestRouter.SupportedActions` and to `ProtocolHandshake.ActionCapabilities`
(and the harness fixture) so the manifest keeps matching what the mod answers.

## Storage

The database is one LiteDB 5.0.21 file per world: `<save dir>/takaro.db` (plus its write-ahead
log `takaro-log.db`), where `<save dir>` is `GameIO.GetSaveGameDir()`, for example
`Saves/Navezgane/MyGame/`. It belongs to that world: a backup or a copy of the save carries it,
and a different world starts its own. `Database.Open` runs as the first `DbWriter` operation at
`GameStartDone` (the save directory is unknown before); the sampler starts right after, and every
write queued later finds the file open.

Every collection has one lifecycle, declared in `Persistence/StorageLifecycle.cs`:

| Collection | Lifecycle | At boot | Kept current by |
|---|---|---|---|
| `items`, `entities`, `locations` | rebuilt at boot | dropped | `SeedOnGameStart` |
| `bans` | rebuilt at boot | dropped | seed, Takaro ban/unban, 60 s resync |
| `inventories` | rebuilt at boot | dropped | join, `SavePlayerData` |
| `players` | durable | every player marked offline | join/spawn, leave, the ~3 s sampler, `players.xml` backfill |
| `waypoints` | durable | kept | reserved for the waypoint commands |
| `settings` | durable | `schemaVersion` written | the connector |

A new collection is added to that table (code and here) before it is used; a collection the file
holds but the table does not know is logged and left alone.

**The player registry is the `players` collection** — there is no second store. A player is
written on every spawn (join, respawn, teleport), marked offline with `LastSeenUtc` on leave, and
the sampler updates the position of online players every ~3 s, so an offline player's record holds
where they were last seen. At boot `Online` is reset for everyone (a crash leaves stale flags), and
players the game itself remembers in `players.xml` but the registry does not (players from before
the registry existed, or after a fresh database) are added; registered players are never
overwritten by that backfill.

**Why the file is opened as two streams.** LiteDB 5 opened by filename fails under Mono with
`ReadFull must read PAGE_SIZE bytes` (seen in #66). The cause is Mono's `FileStream`: it only
bypasses its buffer for writes *larger* than the buffer, and LiteDB writes each 8192-byte page with
one 8192-byte `Write` on a stream with an 8192-byte buffer. The last page of every commit therefore
stays in the writer stream's memory, and a checkpoint, which reads the log back through a second
stream, finds the file one page short. `tests/StorageHarness.cs` reproduces it on every run. The
mod opens the database on two `WriteThroughFileStream`s (`new LiteDatabase(data, null, log)`):
LiteDB then shares one stream per file between its reader and writer, and every write reaches the
operating system before LiteDB's `Write` returns, so a process killed right after a commit loses
nothing. `Flush` (LiteDB calls it at checkpoints) also asks the OS to write to disk. LiteDB stays
on 5.0.21.

If LiteDB cannot read the file, it is moved aside (`takaro.db.unreadable-<UTC>`) and a new one is
started; if the file cannot be opened at all (another server holds it, it is opened with
`FileShare.Read`), the mod runs on an in-memory database and logs that the registry will not
persist. Both are logged as errors.

## Console commands

The mod carries PrismaCore-compatible console commands (`resetlevel`/`rl`,
`resetskillpoints`/`rs`, `isbloodmoon`), so tools written against PrismaCore — StaffCommands among
them — work unchanged. Running PrismaCore next to the connector is unsupported (same command names,
and each ships its own `LiteDB` assembly at a different major version); the mod logs one warning at
game start when a PrismaCore mod is loaded. Every new command follows these conventions:

- **Names:** PrismaCore's plain names and aliases, no `pc-` prefix. The primary name is first in
  `getCommands()`.
- **One file per command family:** `mod/src/Commands/<Family>Commands.cs` holds the game-facing
  classes, each deriving from `PrismaCoreCommand`; `mod/src/Commands/Contract/<Family>Contract.cs`
  holds the game-free part (names, argument rules and every output line, verbatim). The project
  compiles `Commands/**`, and the contract harness compiles `Commands/Contract/*.cs`, so a new
  family adds files and edits neither.
- **Output contract:** errors start with `ERR: ` and are PrismaCore's exact text; arguments are
  validated before a player is resolved; a player that cannot be found prints the game's own line
  (`"<id>" is not a valid entity id, player name or user id.`), which StaffCommands matches on.
- **Players:** resolve them with `PlayerTarget.Resolve` (`Services/PlayerIdResolver.cs` is the
  game-free decision). It accepts entity ids, names and `EOS_`/`Steam_`/`XBL_`/`PSN_` ids; online
  players go through the game's own `ConsoleHelper.ParseParamPartialNameOrId`, everyone else
  through the player registry. A command that needs the live entity checks `IsOnline` and prints
  its offline line otherwise.
- **Threads:** game state is touched only in `Run`, on the main thread. SdtdConsole already runs
  commands there (console, telnet, web dashboard, Takaro's `executeConsoleCommand`);
  `PrismaCoreCommand` marshals any other caller through `MainThreadDispatcher` and waits for it.
  No command starts a thread, uses `Task.Run`, or sleeps; a state change that clients must see is
  pushed with the game's own net packages in the same call (rl/rs send `NetPackagePlayerStats`).
- **Tests:** a contract-harness family in `tests/ContractHarness.<Family>.cs` (a `Family<Name>()`
  method; the harness finds it by name) for the pure logic, and a `takaro-maint verify` family in
  `maintenance/src/takaro_maint/games/seven_days/checks_<family>.py` (listed in `FAMILIES` in
  `verify.py`) that runs the commands through `executeConsoleCommand` and compares their output
  verbatim.

## Configuration

One file: `<server cwd>/Takaro/Config.xml` (`API.BasePath`, next to `Mods/`). The release
zip does not contain it, so replacing `Mods/Takaro/` on an upgrade never touches it. The mod
creates it in `InitMod` (before the world loads) when it is missing, with every key, an empty
`RegistrationToken` and a new UUID `IdentityToken` (mode 0600 on Linux); when the file has no
identity, the mod generates one and writes it back (atomically, keeping the file's comments and permissions).
Nothing else is ever written. A failed write keeps the generated identity in memory and is
retried. There are no environment variables. The dev rig (`dev-servers/lib/render.sh`) and
`takaro-maint verify` render this same file.

The file is read every 5 s (it is tiny, and comparing the text cannot miss an edit); a change
counts once the text has been stable for 1 s, and a half-saved file (invalid XML) is ignored
until it changes again. A change to the URL, a token, the identity or `Enabled` drops the
socket and reconnects at once, so a pasted or corrected token needs no restart. Without a
registration token the mod does not connect (emptying it disconnects); it logs a banner naming
the file instead. `<Chat><ServerChatName>` is the sender name for chat the
connector sends when Takaro passes no `senderNameOverride`; it applies on the next read, without a
reconnect. `src/Services/ServerChatName.cs` (no game types, covered by the contract harness)
resolves the name (override, then `ServerChatName`, trimmed, empty means unset) and renders
`<name>: <message>`; `src/Services/ServerChat.cs` sends it with `EMessageSender.None` and sender
entity id -1, because the client prints no label for that sender. With no name it keeps
`EMessageSender.Server` and the plain text, so the client shows "Server". Any server chat the
connector sends goes through `ServerChat`. `src/Config/ConfigFiles.cs` has no game types and is covered by the
contract harness.

The mod writes its own log to `<server>/Takaro/logs/<M-D-YYYY>.log`. On the dev
rig, DEBUG logging is toggled with the `takaro-debug` console command (it resets
to off on every server restart).

## Versioning

release-please owns `games/7d2d/mod/ModInfo.xml` and `games/7d2d/version.txt` — do not bump them in
a feature branch. Whatever version a PR branch carries is overwritten by the next
release PR, which is why the 2026-09-15 hardening work shipped as release
`7d2d-v0.1.4` even though the branch was labelled 0.1.6.
