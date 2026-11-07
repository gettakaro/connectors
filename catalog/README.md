# The connector catalog

Every connector is built for, and verified against, an **exact** server build. This directory is
where those builds are written down: one JSON record per target, pinned by version *and* by hash,
so a build, a rig and a release all resolve the same bytes.

Nothing here selects "the latest". A new game version is a new record in a pull request.

```
catalog/
  schema/v1/          the JSON Schemas every record is validated against
  <game>/game.json    the game: its download sources, build system and artifact roles
  <game>/targets/     one record per exact server build
```

Read and act on the catalog with the maintenance command:

```
maintenance/bin/takaro-maint targets list --game minecraft
maintenance/bin/takaro-maint targets resolve --game minecraft --target fabric-26.2
maintenance/bin/takaro-maint catalog validate --online
```

## Identifiers

A target id is `<platform>-<revision>`: `fabric-26.2`, `paper-1.21.11`. The file name is the id,
and the record repeats it — all three must agree (`id-matches-stem`). Across games a target is
written `<game>/<id>`, e.g. `minecraft/fabric-26.2`.

`revision` is whatever upstream calls that build: a Minecraft version, a Steam build id, a mod
framework release. It is a string, never parsed for ordering.

## Three kinds of record, kept apart

| Record | Answers | Lives in |
|---|---|---|
| **Observation** | "upstream published X on date Y" | the release-tracking issues write these; never edited by hand |
| **Target** | "we build and verify against exactly this" | `<game>/targets/<id>.json` |
| **Compat record** | "connector version A was proven on target B" | `support.evidence` on the target, and verification reports |

They stay separate because they answer different questions and age differently. Upstream
publishing a new version does not make it a target; declaring a target does not make it verified.
Collapsing them is how a catalog starts claiming support it never earned.

## Input kinds

Every downloadable input declares a `kind`, the `source` it comes from (a key of
`game.json.sources`), the `path` under that source's `baseUrl`, and a hash. `installPath` says
where the file lands under the game directory; an input without one is build-only. An
`installPath`, a component's `installDir` and a build manifest's `file` are relative paths built
from `[A-Za-z0-9._+-]` segments: no leading `/`, no `..`, no backslash, so a record can never
name a file outside the directory the command was pointed at.

| Kind | For | Pinned by |
|---|---|---|
| `mojang-version` | a Minecraft version manifest and the server jar it names | manifest sha1, server sha1 + size |
| `fabric-launcher` | the Fabric server launcher for one (game, loader, launcher) triple | sha256 |
| `maven-artifact` | a jar from a Maven repository, addressed by group/artifact/version | sha256 |
| `http-file` | anything else fetched by URL | sha256 |
| `paper-build` | a PaperMC server jar for one (project, game version, build), fetched by its content-addressed Fill v3 URL | sha256 (the URL's hash segment) + size |
| `neoforge-installer` | the NeoForge server installer for one NeoForge version | upstream `.sha256` sidecar |
| `container-image` | the runtime and build images | immutable tag + manifest digest |

`hashOrigin` records where a hash came from: `upstream` when the publisher serves a checksum we
verified against, `self-recorded` when they do not.

A kind whose acquisition is not one HTTP GET declares no `path` and carries no hash of its own.
It pins a set of files instead — a `files` object of relative path to `{sha256, size}`, every one
of which `no-null-hash` requires — and its provider supplies the identifier that names the set,
which is what reports and compatibility records record in place of a download URL. The kinds
listed above all take the plain-download shape; a new one is added by dropping its schema into
`schema/v1/inputs/`, never by editing `target.schema.json`.

### Trust on first use

FabricMC publishes no checksum for its launcher or its Maven jars. Those hashes are recorded by
downloading the file **twice**, with caching bypassed, and only writing the value when both
downloads agree:

```
maintenance/bin/takaro-maint catalog record-hash \
  --game minecraft --target fabric-26.2 --field inputs.loader.sha256
```

Both downloads' URLs, sizes and hashes go to stderr; the pull request that adds a self-recorded
hash quotes them. After that the hash is the contract: `catalog validate --online` re-downloads
the file and fails on any change, which is exactly what catching a silent re-publish looks like.

## Build systems

A target's `build` block names its `system`, and that name is the schema file under
`schema/v1/build-systems/` the record is validated against — the same dispatch the input kinds use, so a
new build system is a new file there and never an edit to `target.schema.json`.

| System | Builds with | Names |
|---|---|---|
| `gradle` | this repository's Gradle build, one target project per target | `gradleProject`, the Java releases, the plugin versions |
| `script` | a shell script tracked here, inside the pinned container toolchain | `script`, and the `references` it compiles against |

Whatever the block names in this repository has to exist: `gradle-project-exists` for a Gradle
project, `build-script-exists` for a script. `runtime.java` is `null` for a game that ships its
own runtime rather than running on a JRE.

## Icon routes (`icons` block)

A game record may carry an optional `icons` block saying how its Icon Pack is built: `game` (the pack
directory and Takaro game identifier, which can differ from the catalog id), `mapper` (a registered
mapping module), `data` (a `server-depot` input of the target, the depots and file-list selectors to
fetch) and `pixels` (the client app and depot, the decoder, the exact client files in precedence order
and, per revision, the known client build ids; or, with `source: "server-depot"`, just the selectors of pixel files that come down in the same filtered download as the data, so the build needs no game client), plus `secrets`, the names of environment variables the
build reads. Games whose files are Unreal containers add `unreal` (`engine`, the CUE4Parse `EGame` value, and
`aesKeyEnv`, the name of the variable holding the AES key or `null`), `data.decoder: "unreal"` and, for the client
pixels, `pixels.decoder: "unreal"` with `pixels.access: "iostore-ranges"` (the listed `.utoc` files are read whole
and only the `.ucas` byte ranges that hold the needed packages are copied). Unity games set
`data.decoder: "unity"` (a container that reads asset bundles, no settings block) and server-depot `pixels` may
carry the same `decoder: "unity"`; the `icons-server-pixels-decoder` check requires it to equal `data.decoder`,
because both roles are decoded from the one mounted tree. It is validated by `schema/v1/icons.schema.json` and the `icons-*` checks of
`catalog validate`, and it is not part of a target's fingerprint. Details and the commands that read it
are in [maintenance/docs/icons.md](../maintenance/docs/icons.md). The block also makes `takaro-maint scan` add an Icon Pack
rebuild line to that game's new-build issues.

## Fingerprint

The **fingerprint** is `sha256` over a canonical serialization of `id`, `game`, `platform`,
`revision`, `inputs`, `runtime` and `build` — everything that decides which bytes a build and a
deployment use. Support notes, the `default` flag and component descriptions are outside it, so
documenting a target does not invalidate its caches.

Canonical means: keys sorted recursively, no whitespace, integers only (floats are rejected),
minimal JSON string escaping, UTF-8. Two implementations compute it — one in Python
(`maintenance/src/takaro_maint/fingerprint.py`) and one in Kotlin (`TargetCatalog.kt` under the
Minecraft `buildSrc`) — and a shared fixture (`maintenance/tests/fixtures/fingerprints.json`)
proves they agree.

`fp16` (the first 16 characters) keys caches and staging directories. A changed input therefore
never reuses an earlier target's downloads.

## Adding a target

1. Find the exact upstream inputs. Record image digests from the registry
   (`docker buildx imagetools inspect <image>:<tag> --format '{{.Manifest.Digest}}'`), never from
   a locally cached image.
2. Write `<game>/targets/<platform>-<revision>.json` with `support.status: "candidate"` and
   `null` where a self-recorded hash will go.
3. Record the self-recorded hashes with `catalog record-hash` (two agreeing downloads each).
4. Add the build project the record names — for Minecraft, `games/minecraft/mod/targets/<id>/`.
5. `takaro-maint catalog validate --online` must exit 0.
6. Verify it: `takaro-maint build` then `takaro-maint verify`. Promote to
   `support.status: "maintained"` only with the report to show for it, and cite that evidence in
   `support.evidence`.

Exactly one target per (game, platform) carries `default: true`. Zero or two is an error, because
a command with no `--target` would otherwise pick silently. A game with several platforms also
names a `defaultPlatform` in its `game.json`: that is the platform `--game` alone resolves to,
and it has to be one of the game's `platforms`. `--platform` still takes any platform's default.

## Retiring a target

Set `support.status: "retired"`. A retired target is left out of build matrices and the generated
README table, and its hashes are no longer re-checked online — but the record stays, so a report
that cites it still resolves. Delete the Gradle project separately, once nothing builds it.

## Where versions live

Game-coupled coordinates — the Minecraft version, the Fabric loader, the Fabric API — live
**here**, not in Gradle files. They change per target and only through a catalog pull request,
reviewed with the evidence that the new build actually works.

Renovate keeps the things that are not coupled to a game version: Loom, shadow, ModDev, the
Gradle wrapper, GitHub Actions and base images.
