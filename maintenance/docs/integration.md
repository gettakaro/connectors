# Private integration

The contract a private deployment follows when it consumes this repository. It is written for
the other side: a deployment repository, a compose stack, a cluster job — anything that installs
and runs a connector it did not build.

The whole of it is one rule with five steps behind it: **the deployment carries no version of
its own.** Every image, every revision, every artifact name and every digest comes from
`takaro-maint targets resolve` or from a release's compatibility record. What follows is how to
get them.

## Purpose

A deployment that keeps its own pins drifts. `image: takaro/server:latest` is a different
server every week; `app_update 294420` without a build id is whatever Steam published this
morning; a hand-typed `1.4.5.6` in a compose file is right until the catalog moves and nobody
edits it. The catalog already answers all three questions exactly, and a fingerprint ties the
answer together, so the deployment's job is to ask rather than to remember.

Concretely, a private side that follows this contract has:

- no `:latest`, no floating tag, no image without a digest;
- no `app_update` to a branch head and no `production_build` alias;
- no `-SNAPSHOT`, no `beta` or `stable` alias resolved at deploy time;
- no version string typed anywhere except as the `--target` it asks for.

## Resolve

```
takaro-maint targets resolve --game G [--target ID | --platform P] \
    --format json|env|gha [--prefix P] [--out FILE]
```

Selection is the same as everywhere else: `--target` names one exactly and wins; without it the
single record with `default: true` for that game (and `--platform`, when given) is used. Zero or
two defaults is an error — exit `3` — not a guess.

**`--format json`** (the default) prints the whole target record plus the resolution:

| Key | What it is |
|---|---|
| `id`, `game`, `platform`, `revision`, `default` | The record's identity. |
| `fingerprint`, `fp16` | The hash over everything that makes this target this target, and its 16-character short form. Two things that agree on `fp16` are the same target. |
| `inputs`, `build`, `runtime`, `support`, `preserve` | The record as written. |
| `artifactFileNames` | `{role: filename}` — the exact name each built artifact has. |
| `containerRef` | The runtime image as `image:tag@digest`. |
| `toolchainRef` | The build toolchain image, likewise pinned by digest. |
| `resolvedUrls` | Every pinned input's download URL. |
| `env` | The deployment environment: the keys below. |

**`--format env`** prints `env{}` alone, one sorted `KEY=VALUE` line per key, ready for a
compose `--env-file` or a systemd `EnvironmentFile`. With `--out FILE` it is written with mode
`0600`. Keys are prefixed `TAKARO_TARGET` by default; `--prefix` changes that.

**`--format gha`** prints the matrix keys a workflow consumes — `target`, `game`, `platform`,
`revision`, `fingerprint`, `fp16`, `image`, `toolchain`, `java`, `build_system`, and
`gradle_project` for a Gradle build — followed by one `env=<json>` line carrying the same `env{}`
as compact JSON.

## Install

```
takaro-maint install --game G [--target ID] --dest DIR \
    [--reuse-world | --fresh-world] [--dry-run] [--rollback]
```

Installation is **stage, verify, swap**: every pinned input is downloaded and checked against
its recorded sha256 and size into a staging directory, and only a complete, verified set is
moved into `--dest`. A failed download leaves the running install untouched.

- The files a deployment owns are named by the record's `preserve[]` — configuration, worlds,
  logs — and are never replaced or removed by an install.
- `--dry-run` reports exactly what would change and writes nothing.
- The ledger `<dest>/.takaro/installed-target.json` is written **last**, so its presence means
  the install completed. Nothing may infer the installed version from a filename.
- Steam-delivered games keep the previous install as `.previous`, and `--rollback` restores it.

Run `ledger check` before every start:

```
takaro-maint ledger check --game G [--target ID] --dest DIR
```

It answers one question — does this directory really hold that target, with the recorded bytes
intact? — and a nonzero answer means do not start the server.

## Artifact validation

Every artifact this repository builds carries its own identity, so a deployment never has to
trust a filename:

```
takaro-maint artifact validate --game G --target ID FILE…
```

For a jar the identity is in the manifest — `Takaro-Target`, `Takaro-Target-Fingerprint`,
`Takaro-Connector-Version`, `Takaro-Source-Revision` — and for other artifact kinds in a sidecar
`.meta.json`. A file whose fingerprint is not this target's is refused with exit `7`, which is
what catches "the right-looking jar for the wrong revision".

At release level the equivalent check is:

```
takaro-maint release verify --tag TAG [--connector C] [--expect DIR]
```

It downloads every asset of the release and proves the set is exactly what its compatibility
record describes.

## Deploy

```
takaro-maint deploy --game G [--target ID] --dest DIR --from build-manifest.json
```

The artifact is chosen by **(target, role)** from the build manifest, never by a name a human
typed. Deploy refuses a directory whose ledger holds another target or another fingerprint
(exit `7`), replaces an older artifact of the same role, and records what it placed in the
ledger.

## Verify

```
takaro-maint verify --game G [--target ID] --artifacts DIR --out DIR [--takaro local|hosted]
```

`--takaro local` boots the target against an in-process fake Takaro and needs nothing from you.
`--takaro hosted` runs the same checks against a real Takaro and reads its connection details
from the environment only:

`TAKARO_WS_URL`, `TAKARO_REGISTRATION_TOKEN`, `TAKARO_HOST`, `TAKARO_USERNAME`,
`TAKARO_PASSWORD`, `TAKARO_DOMAIN_ID`.

The report written under `--out` validates against `catalog/schema/v1/verify-report.schema.json`
and states which checks ran and at what level. [Runtime verification](verify.md) describes the
checks themselves.

## The compatibility record

Every release ships one, `takaro-<connector>-<version>.compat.json`, and it is the document a
deployment reads:

```
schemaVersion, kind: "compat-record", connector, version, channel, tag, mode, generatedAt,
tool {name, version},
source {repo, commit, tag, dirty, catalogSha256},
catalog, targets {…}, aliases, assets, checksums: "SHA256SUMS", self
```

`targets[<id>]` holds that target's `platform`, `revision`, `status`, `fingerprint`, its pinned
`inputs`, the `runtime` image (`image`, `tag`, `digest`), the `verification` block and the
`artifacts` for each role with their `sha256`, `size` and `url`.

A deployment therefore:

1. reads `targets[<its target id>]` — not an alias, not a filename;
2. downloads that target's artifacts and checks them against `SHA256SUMS`;
3. reads `verification.required` and `verification.executed` and **refuses the release when
   `executed` is weaker than `required`** (the ranks are `none < build < contract < startup <
   protocol < gameplay`; `executed: null` means nothing ran);
4. uses `runtime.digest` as the image to run.

`aliases` exist only for older consumers that expect legacy asset names; new integrations read
`targets` and ignore them.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | ok |
| 1 | unexpected error (a traceback is printed) |
| 2 | usage, configuration or catalog invalid |
| 3 | target not found, or an ambiguous default |
| 4 | an upstream input or provider is unavailable |
| 5 | integrity mismatch — the bytes are not what the catalog pins |
| 6 | the build failed |
| 7 | artifact, ledger or asset conflict (wrong target, stale fingerprint, incomplete set) |
| 8 | verification failed, or its evidence is missing |
| 9 | tracker/GitHub failure, or missing authentication |
| 10 | icon bucket read or upload failed |
| 130 | interrupted |

## The sequence

One deployment, start to finish. Every value comes out of the previous step; nothing is typed
twice.

```sh
set -euo pipefail
GAME=minecraft
TARGET=fabric-26.2
DEST=/srv/$GAME

# 1. resolve — the target's environment, written where compose can read it
takaro-maint targets resolve --game "$GAME" --target "$TARGET" \
    --format env --out "$DEST/.takaro/target.env"

# 2. install — stage, verify, swap; the ledger is written last
takaro-maint install --game "$GAME" --target "$TARGET" --dest "$DEST" --reuse-world

# 3. validate — the artifacts really are this target's
takaro-maint artifact validate --game "$GAME" --target "$TARGET" dist/*.jar

# 4. deploy — by (target, role), from the build manifest
takaro-maint deploy --game "$GAME" --target "$TARGET" --dest "$DEST" \
    --from dist/build-manifest.json

# 5. verify — and keep the report
takaro-maint verify --game "$GAME" --target "$TARGET" --artifacts dist --out reports

# 6. before every start, and after every change
takaro-maint ledger check --game "$GAME" --target "$TARGET" --dest "$DEST"

docker compose --env-file "$DEST/.takaro/target.env" up -d
```

## What the private side removes

| Remove | Replace with |
|---|---|
| `image: something:latest` or any undigested tag | `${TAKARO_TARGET_IMAGE}` from `--format env`, i.e. the record's `containerRef` (`image:tag@digest`) |
| `steamcmd +app_update <app>` to a branch head, or a `production_build` alias | `takaro-maint install`, which resolves the pinned depot manifests |
| A hand-typed game version in a compose file, a script or a README | the target's `revision`, read from the resolution |
| A jar chosen by filename or by "the newest one" | `takaro-maint deploy --from build-manifest.json`, by (target, role) |
| "Download the latest release" | the tagged release's compatibility record, plus `release verify` |
| A `-SNAPSHOT` or `beta`/`stable` alias resolved at deploy time | the target id, which is stable and reviewed |

Nothing in this document names a host, a path or a domain on the private side: the contract is
what this repository offers, and where that is consumed is not this repository's business.
