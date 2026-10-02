# Releases: publishing a connector as one complete, checkable set

A connector is not a jar, it is a set: every non-retired target, every role the catalog
declares for it, each one built from the same commit and each one backed by a verification
report that says it actually worked. `takaro-maint release` assembles that set, publishes it,
and reads it back from GitHub before anyone can download it. If any part of the set is missing,
stale or unverified, nothing is published at all.

That is the whole point. A release page that holds three of four jars, or a jar whose
verification was never run, looks exactly like a good release to the person downloading it.

## The three commands

```
takaro-maint release assemble --connector C --version V --channel stable|rolling|pr --tag TAG \
                              --dist DIR --out DIR [--reports DIR] [--mode catalog|legacy] \
                              [--source-commit SHA] [--allow-dirty] [--repo OWNER/REPO] [FILE…]

takaro-maint release publish  --connector C --channel stable|rolling|pr --tag TAG --assembled DIR \
                              [--repo R] [--api-url U] [--token T] [--target-commit SHA] \
                              [--run-id ID] [--pr-number N] [--title T] [--notes-file F] \
                              [--no-finalize]

takaro-maint release verify   --tag TAG [--connector C] [--expect DIR] [--repo R] [--api-url U] \
                              [--token T] [--out DIR]
```

`scripts/publish-release.sh publish` runs all three in order and is what every workflow calls.

### Channels

| Channel | Tag | What it is |
|---|---|---|
| `stable` | `<connector>-v<version>` | The release-please release. Created as a **draft** with the tag already forced; the publisher fills it and un-drafts it. |
| `rolling` | `<connector>-dev` | Replaced on every push to main. Swapped in through a staging draft. |
| `pr` | `pr-<n>-<connector>` | Replaced on every push to a PR, deleted when the PR closes. Also a staging swap. |

### Modes

`catalog` is the default for a connector that has `catalog/<connector>/game.json`: the set is
derived from the per-target build manifests and the per-target verification reports, and the
asset names come from the catalog. `legacy` is for a connector that has not moved to the
catalog yet: the files are named on the command line and published exactly as they are.

## Asset naming

**Catalog connectors.** One asset per (target, role), named by the catalog's
`components[].artifact` with `{version}` substituted:

```
takaro-<connector>-<role-word>-<target-id>-<version>.<ext>
```

For Minecraft 0.1.2 that is

```
takaro-minecraft-mod-fabric-26.2-0.1.2.jar
takaro-minecraft-mod-fabric-26.1.2-0.1.2.jar
takaro-minecraft-mod-paper-1.21.11-0.1.2.jar
takaro-minecraft-mod-neoforge-1.21.11-0.1.2.jar
```

The name is computed, never globbed, and has to equal the name in the build manifest. A build
that produced a differently named file does not assemble.

**Legacy aliases.** `game.json`'s `legacyAssetAliases` maps an old asset name (with
`{version}`) to `"<target-id>/<role>"`, and the publisher ships a byte-identical copy under
that old name so existing download links keep working. Aliases are kept for two stable releases
and then removed in a catalog PR. An alias that points at a retired or absent target is skipped
and listed in `aliasesSkipped`. An alias key is one file name, never a path, and no two assets in
a set may want the same name — including `SHA256SUMS` and the record itself.

**Legacy connectors.** Exactly the names their build scripts produce today, unchanged:

| Connector | Assets |
|---|---|
| rust | `TakaroConnector.cs` |
| 7d2d | `takaro-7d2d-mod.zip` |
| zomboid | the shadow jar in `dist/` |
| valheim | `takaro-valheim-plugin.zip`, `takaro-valheim-inventory-companion.zip` |
| conan-exiles | `takaro-conan-exiles-bridge.zip` |
| terraria | `takaro-terraria-plugin.zip`, `takaro-terraria-bridge.zip` |
| enshrouded | `takaro-enshrouded-plugin.zip` (and `takaro-enshrouded-sidecar.zip` up to 0.5.0) |

**Always.** `SHA256SUMS` (GNU format, sorted, covering every asset but itself),
`takaro-<connector>-<version>.compat.json`, and one
`takaro-<connector>-<version>.verify-<target-id>.json` per verified target.

Every download link is
`https://github.com/<owner>/<repo>/releases/download/<tag>/<name>`. For a draft the link only
starts working when the release is published.

## The compatibility record

One JSON document per release, validated against
`catalog/schema/v1/compat-record.schema.json` before it is written. It is what answers "which
jar do I want, and was it actually tested?" without the repository at hand.

| Field | What it holds |
|---|---|
| `connector`, `version`, `channel`, `tag`, `mode` | Which release this is. |
| `generatedAt`, `tool` | When it was written, and by which version of this command. The stamp comes from `SOURCE_DATE_EPOCH`, else the commit time of `source.commit` — never from the clock, so two assemblies of one commit are byte-identical. |
| `source.repo`, `source.commit`, `source.tag`, `source.dirty` | The repository and commit every artifact was built from; `tag` is set for stable releases only. `dirty` is true if the checkout that assembled, any build that produced an artifact, **or any verification report shipped with the set** had uncommitted changes under the connector's paths. |
| `source.catalogSha256` | One hash over `game.json` and every non-retired target record, canonicalised the way a fingerprint is, so a checkout can recompute it. |
| `catalog` | `{game, targetIds}`, or `null` in legacy mode. |
| `targets.<id>.platform`, `.revision`, `.status`, `.fingerprint` | Which server this target is, and the fingerprint the artifacts carry. |
| `targets.<id>.inputs` | Every pinned input by name: kind, URL and the hash or size the catalog pins. |
| `targets.<id>.runtime` | The container image, tag and digest the target runs in. |
| `targets.<id>.verification` | `required`, `executed`, `report`, `outcome`, `takaro`. |
| `targets.<id>.artifacts` | Per role: name, sha256, size and download URL. |
| `aliases` | Old asset name → the target, role, source asset and hash it copies. |
| `assets` | Every asset but `SHA256SUMS` and the record itself, with its kind. |
| `checksums`, `self` | `"SHA256SUMS"` and the record's own file name. |

## A stable release, end to end

1. A maintainer merges the connector's Release PR. **This is the only human gate, and it is
   unchanged.**
2. release-please creates the release as a **draft** and forces the tag
   (`"draft": true, "force-tag-creation": true` in `release-please-config.json`). Without
   `force-tag-creation` GitHub creates no tag for a draft, and there would be nothing to check
   the checkout against.
3. The connector's workflow starts, checks out **the tag** (not main), and `release-guard.sh`
   requires the tag, the version, `.release-please-manifest.json`, `games/<c>/version.txt` and
   the checked-out commit to all agree. The tag lookup is retried, because release-please
   sometimes pushes the tag a second after dispatching the workflow.
4. Every target builds in parallel in its pinned toolchain container — twice, with
   `--rerun-tasks`, requiring identical bytes.
5. Every target boots in its pinned runtime container and is verified.
6. One `publish` job runs `release assemble`, `release publish` and `release verify`.
7. The release is un-drafted. Only now is any of it downloadable.

## What blocks a release

| Exit | Cause |
|---|---|
| 2 | Usage: `--out` is not empty, no compatibility record in `--assembled`, `--allow-dirty` on a stable channel, positional files outside legacy mode, or a `legacyAssetAliases` key that is not a plain file name. |
| 5 | Bytes disagree with a hash that was already written down — a built file against its build manifest, or a file in the assembled set against `SHA256SUMS`. Nothing is uploaded. |
| 7 | The set is wrong: a missing (target, role), two different builds of the same one, a fingerprint the catalog no longer has, a file name the catalog does not predict, two assets wanting one name, a build manifest from another commit or from a dirty tree, a verification report from a dirty tree, a report that describes different bytes, a dirty tree, an assembled set built for another repository or commit, a record that does not claim the tag it sits on, a tag that points somewhere else, or an asset already on the release with different bytes. |
| 8 | The evidence is missing or negative: no report for a target that requires one, `outcome: fail`, a level below what the target requires, or a target requiring `gameplay` — which this harness does not produce. |
| 9 | GitHub: no token, or a failed request. |

`missing` in the exit-7 document names every `{target, role}` that was not built, so the failure
says which build leg to look at.

## Recovery

If a release ends up without its assets — a cancelled run, a runner that died mid-upload — it is
recovered by dispatching the connector's workflow against the existing tag:

```
gh workflow run minecraft.yml --ref main -f tag=minecraft-v0.1.2 -f version=0.1.2
```

The run checks out `minecraft-v0.1.2`, so the artifacts are rebuilt from the source that tag
names rather than from whatever main has become since. It re-runs `release assemble` as well, and
that re-assembly has to produce the same bytes as the interrupted one did — otherwise the retry
would conflict with whatever the first run managed to upload. Nothing in the set is allowed to
depend on when it was built: for a catalog connector the archives are packaged through
`scripts/lib/package.sh` (or by the Gradle build's reproducible-jar settings, proven twice per
build leg) and the compatibility record is stamped from the source commit's own time. Then:

* an asset that is already there with **identical** bytes is `skipped-identical` — no upload,
  no delete;
* an asset that is there with **different** bytes is a conflict: exit 7, and nothing is
  uploaded — every remote asset is compared before the first upload, so a release that is
  already public is left exactly as it was;
* a release that is already published stays published (`alreadyPublished: true`), and no
  published release or tag is ever deleted on the stable path.

A tag created before this machinery existed cannot be recovered this way: its tree has no
`catalog/` or `maintenance/` to build from.

**Legacy-mode connectors** (`--mode legacy`: 7d2d, rust, zomboid, valheim, conan-exiles,
terraria, enshrouded) do not yet package through `scripts/lib/package.sh`, and their
builds have not been proven byte-reproducible, so a recovery rebuild of one of them can produce
different bytes from the interrupted run's. The publisher then stops with exit 7 and uploads
nothing, exactly as above. Because a stable release is still a draft until the publisher
finishes it, the way forward is to delete the draft's partial assets and dispatch again:

    gh release view 7d2d-v1.2.3 --json assets -q '.assets[].name'
    gh release delete-asset 7d2d-v1.2.3 <name> --yes      # once per listed asset
    gh workflow run 7d2d.yml --ref main -f tag=7d2d-v1.2.3 -f version=1.2.3

Never delete assets from a release that is no longer a draft. Each connector adopts the
deterministic packaging in its own maintenance issue; until then this is the recovery path.

## Rolling and PR builds

These have no draft to fill, so the publisher builds the entire replacement first:

1. Delete any staging draft left by a dead run of the **same** tag (`<tag>.staging-<run-id>`).
2. Create a new staging draft at `<tag>.staging-<run-id>` and upload every asset to it.
3. Verify it from GitHub.
4. Only then delete the old release, delete the old tag, and rename the staging draft to `<tag>`.
   Every step of that swap reports the staging draft by name if it fails, and says whether the
   old release was already removed, so the complete set is never left unfindable.

Until step 4 the previous build is intact and complete; a failure anywhere before it deletes the
staging draft and leaves the old release exactly as it was. Nothing outside this tag's
`.staging-` prefix is ever deleted, so two connectors — or two PRs — publishing at the same time
cannot reach each other's releases.

**The one window.** If the final rename fails after the old release has been deleted, the
complete new set exists as a staging draft and the command says so and exits 9. Re-running the
workflow removes that draft and republishes. This is a single API call wide and is the price of
not keeping two releases for one tag.

## Deterministic packaging

`scripts/lib/package.sh` removes the four things that make two honest builds of the same commit
produce different archives: entry order, checkout mtimes, the builder's umask and gzip's header
timestamp. `SOURCE_DATE_EPOCH` decides the timestamp, falling back to the repository's own
commit time.

```bash
. scripts/lib/package.sh
pkg_zip    "$STAGE" TakaroPlugin dist/takaro-plugin.zip
pkg_tar_gz "$STAGE" TakaroPlugin dist/takaro-plugin.tar.gz
pkg_sha256sums dist
```

`maintenance/tests/test_package_determinism.sh` builds the same content in two different paths,
in the opposite order, with different mtimes and permissions, and requires identical bytes — and
requires a one-byte content change to change them. The library is available to the game build
scripts; adopting it — and proving the build itself reproducible by building twice and comparing,
as the Minecraft legs do — belongs to each game's own maintenance issue. Until a connector has
done both, its stable recovery is the manual path described under Recovery.

The Minecraft build legs prove the same thing for the Gradle build directly, by building each
target twice with `--rerun-tasks` and comparing the jars.

## A local dry run

Against a scratch repository of your own, never the real one:

```bash
maintenance/bin/takaro-maint release assemble --connector minecraft --version 0.1.2 \
    --channel stable --tag minecraft-v0.1.2 --dist dist --reports reports --out assembled \
    --repo <you>/<sandbox>

maintenance/bin/takaro-maint release publish --connector minecraft --channel stable \
    --tag minecraft-v0.1.2 --assembled assembled --repo <you>/<sandbox> --no-finalize
```

`--no-finalize` uploads and verifies but leaves the release a draft. Create the tag and a draft
release on the sandbox first — that is exactly the state release-please leaves behind.

## Not yet proven

The first production stable cut has not happened. Until a Minecraft Release PR merges and the
workflow finalises the draft, the draft/forced-tag path is proven by command-level tests and by
a run against a private sandbox repository, not by a real release. The recovery dispatch against
a real published tag is owed by the same lane.
