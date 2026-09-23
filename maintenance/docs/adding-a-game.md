# The provider contract: adding a game

Every connector after the first two is meant to arrive as **catalog data**, not as code.
This document is the contract that makes that true, and
`tests/test_provider_contract_reuse.py` is its executable form: it builds a whole game
that exists only in a temporary catalog, scans it end to end, and then asserts that no
module under `src/takaro_maint` mentions the game at all.

## What a game adds

Every connector registered by `games/*/connector.json` or a `games/*` release-please
package must have one catalog game with an enabled `kind: game` watcher. The watcher
must name the catalog game as its component and use a supported observation provider.
Run `maintenance/bin/takaro-maint catalog check-maintenance` before submitting it.
The unfiltered Lint workflow runs this check on every PR, including PRs that only add
connector files and forget the catalog entirely.

Merging catalog changes to `main` starts maintenance automatically. Both this run and
the six-hour schedule pass `--bootstrap`: new sources initialize their checkpoints,
record covered releases, and file only uncovered current heads. Existing sources
continue from their checkpoints, processing unseen releases normally. No manual
bootstrap dispatch is needed. The `publish_schedule` switch pauses both automatic
triggers; manual read-only runs remain read-only.

A discovery-only catalog game can start without exact targets or a build adapter.
Vein and Dragonwilds use this arrangement until their exact targets are added. Their
initial uncovered heads become maintenance work; discovery does not claim verified
compatibility or add targets automatically.

| File | What it holds |
|---|---|
| `catalog/<game>/game.json` | the sources it downloads from, the watch blocks it observes, its platforms and artifact roles |
| `catalog/<game>/targets/<platform>-<revision>.json` | one exact, hash-pinned target |
| `maintenance/src/takaro_maint/games/<game>/` | the adapter (`__init__.py`, subclassing `games.base.BaseAdapter`) and its `verify.py`, which ends in one `HOOKS = GameHooks(...)` |
| `games/<game>/scripts/lib-target.sh` | a shim over `scripts/lib/target.sh`: `<g>_repo_root`, `<g>_resolve_target`, `<g>_parse_target_flag`, plus anything only this game's scripts need |
| `dev-servers/lib/games/<game>.sh` and `dev-servers/<game>.yml` | the local rig |
| `.github/workflows/<game>.yml` | its build, on the shared `connector-release.yml` wrapper |

## What a game never touches

The core Python: the providers, the schemas, the scan, the tracker, the reconciliation.
A game that needs a change there is telling you the contract is missing something —
extend the contract, not the game's own branch of it.

A game adapter is *not* an exemption from this. It holds what is genuinely specific to one
game (how its mod is built, how its server is verified). Acquisition and discovery come
from a provider, and `scan` never asks for an adapter at all: a game with a watch block
and no adapter scans perfectly well.

What the adapter and its hooks may contribute is a closed set rather than whatever name
the core happens to look for. The adapter subclasses `games.base.BaseAdapter` and
overrides only the hooks it needs -- `install`, `after_deploy`, `container_mounts`,
`container_options`, `container_command` -- each of which has a default there, so a
misspelled or renamed hook is a type error rather than a hook that is silently never
called. Verification goes the same way: `games/<game>/verify.py` keeps its module-level
constants and ends with one `HOOKS = GameHooks(...)` naming the ready line, the check ids
this game adds, the base checks it cannot pass (`UNSUPPORTED_CHECKS`, which the runner
uses to narrow a `--checks`-less run) and whichever of the lifecycle callables it
implements. A `verify.py` without a `HOOKS` object is refused, not treated as a game with
no hooks. [Runtime verification](verify.md) has the field list.

## Input kinds

An input's `kind` names the schema that checks it, and the schema is the only registry
there is — an unknown kind fails `catalog validate` rather than passing unchecked. The
kinds available today:

| Kind | What it pins |
|---|---|
| `mojang-version` | a Minecraft version manifest and its server jar |
| `fabric-launcher`, `maven-artifact`, `paper-build`, `neoforge-installer` | the Minecraft framework layers |
| `steam-depots` | a whole dedicated-server install, as one content manifest per Steam depot |
| `github-release-asset` | one file published on a GitHub release |
| `thunderstore-package` | one versioned Thunderstore package |
| `github-source` | a source tree at one exact commit |
| `http-file` | anything else fetched by URL and pinned by sha256 |
| `container-image` | an image by immutable tag **and** digest |

Every one of them refuses a floating coordinate. `tag: "latest"`, a two-part Thunderstore
version and a branch name where a commit belongs are all rejected — by the pattern in the
schema, or by the catalog-wide `no-floating-words` check, or both. What downloads today
has to be what downloads next year.

## Watch blocks

A source becomes observable by growing a `watch` block. The keys every provider shares:

| Key | Meaning |
|---|---|
| `kind` | `game` (the server itself), `framework` (something the connector builds against), or `branch-review` (produced, never declared) |
| `component` | the marker component, conventionally the game id |
| `channels` | the upstream labels that are watched, each mapped to a marker `branch`; `enabled: false` means "declared and deliberately unwatched", which is different from "unknown" |
| `window` | how far back in a listing to look |

Channel keys are the upstream vocabulary, verbatim, so no code has to translate: Paper
names a `channel` enum, NeoForge a version `suffix`, Fabric a `stable` boolean, GitHub a
tag, Steam a branch label.

### `github-release`

```json
"carbon-api": {
  "provider": "github-release",
  "baseUrl": "https://api.github.com",
  "watch": {
    "kind": "framework",
    "component": "rust",
    "repo": "CarbonCommunity/Carbon",
    "window": 10,
    "channels": {
      "production": {
        "branch": "release",
        "tag": "production_build",
        "asset": "^Carbon\\.Linux\\.Release\\.tar\\.gz$",
        "versionPattern": "v([0-9][0-9.]*)",
        "mutable": true
      }
    }
  }
}
```

| Key | Meaning |
|---|---|
| `repo` | `owner/name` |
| `tag` | an exact tag, or `regex:<pattern>`; omitted means every release in the window |
| `prerelease` | require the release to be (or not to be) a prerelease |
| `asset` | **required**, a regular expression over the asset names. Matching zero or two assets fails the source rather than guessing: which of `Release`, `Minimal` and `Debug` a connector links against is not a detail to pick for someone. |
| `versionPattern` | a regular expression with one group, applied to the release *name*, for projects whose real version is in the title and not in the tag (`Production Build — v2.0.259`) |
| `mutable` | the tag is re-uploaded in place |

`mutable` is the interesting one. Carbon's tags (`production_build`, `edge_build`) are
rolling: the same tag gets a new build whenever one is cut. A scan keyed on the tag would
file one issue in the project's lifetime and then go quiet, so a mutable channel folds the
asset's published digest into the revision — `production_build.bfc3cf3d` — and reports its
history as `heads-only`, because the uploads between two scans were never seen and are
not claimed.

Acquisition uses a **second** source pointing at the download host
(`https://github.com`), exactly as Fabric splits `meta` from `maven`: the API listing and
the asset download are different hosts, and the catalog stays the only place a host is
written down.

### `steam`

```json
"steam": {
  "provider": "steam",
  "baseUrl": "https://store.steampowered.com",
  "watch": {
    "kind": "game",
    "component": "7d2d",
    "app": 294420,
    "os": "linux",
    "depots": ["294422"],
    "channels": {"public": {"branch": "public"}},
    "knownBranches": ["regex:^v[0-9]+(\\.[0-9]+)*$"]
  }
}
```

The channel key is the upstream branch label verbatim and `branch` is the marker name,
because Steam's labels (`latest_experimental`) and the marker alphabet (no underscore) do
not agree. `depots` is what makes the identity: a build is an app on a branch at a build
id with one content manifest per depot, and Steam is `heads-only` — it publishes no
history at all. The whole contract, including protected branches and what a revision
means, is in [Steam discovery](steam-discovery.md).

## Proving it

`tests/test_provider_contract_reuse.py` is the test to extend when a new mechanism lands.
It does three things, and the third is the one that matters:

1. checks each input kind through the real `catalog validate`, including that a floating
   coordinate fails;
2. scans a game that exists only as a `game.json` in a temporary catalog copy — no
   targets, no adapter, no fixtures of its own — and asserts the observation it produced;
3. greps every module under `src/takaro_maint` for the game's name and requires no hits.

If step 3 ever fails, something game-specific has leaked into the core, and that is the
bug — not the test.
