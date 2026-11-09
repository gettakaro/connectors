# Steam exact install

A Steam-delivered dedicated server has no download URL and no stable archive. A build is an
app on a branch plus one content manifest per depot, and the branch head moves whenever the
publisher pushes — so "install the server" means "install exactly these manifest ids", or the
same command installs different bytes on different days.

This is how `takaro-maint` does that, and what it refuses to do.

## The input kind

`catalog/schema/v1/inputs/steam-depots.schema.json` pins one installation:

| Field | What it pins |
|---|---|
| `app`, `branch`, `buildid` | Which published build this is. |
| `os`, `arch` | Which platform's depot files to ask for. |
| `depots` | One content manifest id per depot, with its size and file count. **This is what is downloaded.** |
| `files` | The declared files: relative path → sha256 + size. What `install` verifies, what the ledger records, and what proves after a boot that nothing replaced them. |
| `credentials` | Environment variable *names* only (`branchPasswordEnv`, `accountEnv`), or `null` for the anonymous login. A credential value never enters the catalog. |

The declared files are a deliberate subset: the launcher and the assemblies that identify the
build. Hashing 17 000 files on every install would cost minutes and prove nothing more.

## The tool

`maintenance/tools.lock.json` pins DepotDownloader by url, size and sha256. Bytes that do not
match the lock are refused rather than run (exit 5); the archive is unpacked once into
`<cache>/tools/depotdownloader/<version>-<platform>-<archive sha256 prefix>/` and reused.
"Already there" is not the guarantee the lock makes, so every run re-hashes the cached
executable against `.takaro-tool.json`, the record written when it was unpacked; an edited,
half-replaced or wrong-platform binary is fetched again rather than run.
`TAKARO_MAINT_DEPOTDOWNLOADER` points at another executable, which is how the tests drive the
real code path with no network.

A zero exit is not proof either. After every download the tool's own two records — the
`Manifest <id> (<date>)` line it prints and the `<depot>_<id>.manifest` file it leaves in the
download directory — are read back, and a run that served anything but the pinned manifest is an
upstream failure (exit 4), never content cached under the pinned manifest's key.

## The cache

```
<cache>/steam/<app>/<depot>/<manifest>/          one depot manifest, downloaded once
<cache>/steam/<app>/<depot>/<manifest>/.takaro/complete.json
<cache>/steam/logs/<game>-<target>.log           every tool invocation and its output
```

`complete.json` is written last, so a partial download is never mistaken for a cache hit. A hit
re-hashes the declared files it holds; a corrupt cache is deleted and downloaded once more, and a
second failure is an integrity error rather than a third attempt.

Game bytes never leave the machine: the cache is local, and a release contains the connector
artifact alone.

## The install

`takaro-maint install --game G --target ID --dest DIR`

1. **Fast path.** The ledger already names this fingerprint and every declared file still hashes
   as recorded → `already-installed`, nothing downloaded, nothing written.
2. **Fetch.** Each pinned depot manifest into the cache. Never the branch head, never another
   manifest.
3. **Stage.** The whole tree is copied to `<dest>.staging-<fp16>`, beside the install and on the
   same filesystem.
4. **Verify.** Every declared file's sha256 and size, in staging. A mismatch exits 5 and says
   the existing install is untouched — because it is: nothing has been swapped yet.
5. **Preserve.** The target's `preserve[]` entries are carried from the existing install into
   staging (configs, the deployed mod, the ledger).
6. **Adapt.** The game adapter's `post_install` hook runs — for 7 Days to Die that writes
   `DONT_REMOVE.txt`, which is what stops the server image's own installer from replacing the
   pinned build.
7. **Swap.** `<dest>` becomes `<dest>.previous`, staging becomes `<dest>`. Two renames.
8. **Ledger last.** `<dest>/.takaro/installed-target.json` records the fingerprint, every
   declared file as installed, and the container image.

Steps 7 and 8 are one protected window. Anything that fails inside it moves the install that was
in service back to `<dest>` — a machine is never left with no install at all, and never with the
new tree live under the previous install's identity. Anything that fails before the swap removes
the staging directory and leaves `<dest>` byte-identical, which the tests assert with a tree hash.

`--dry-run` reports the depots and files it would fetch and writes nothing.

`--rollback` swaps `<dest>.previous` back: exit 7 when there is none or it carries no ledger, and
exit 5 when it no longer hashes as its own ledger recorded — a damaged previous install is not put
into service, and `<dest>` is left alone. The declared files of the *currently selected* target are
reported in `problems`, because the restored install predates that target and is expected to
differ from it.

## Re-pinning: `steam pin`

```
takaro-maint steam pin --game G [--target ID] [--branch B] [--buildid N]
                       [--depot D …] [--record-files PATH …] [--write]
```

Reads the manifest listing of each depot on the branch (no content downloaded), prints what Steam
serves now, and lists in `changed` the depots whose manifest no longer matches the pin. `--write`
replaces `inputs.<name>` in the target record and nothing else.

A pin that moves invalidates every recorded file hash, so `--write` refuses unless each declared
file was re-recorded with `--record-files PATH` — which downloads exactly those files *from the
new manifest* and hashes them. Recording old bytes under a new manifest id is the drift this
command exists to prevent.

## Build references: `steam references`

```
takaro-maint steam references --game G [--target ID] --dest DIR [--force] [--reuse-by-manifests]
```

Downloads only the files `build.references` selects (a DepotDownloader filelist; plain paths or
`regex:…`) from the pinned manifests, verifies each against the target's declared hashes, and
flattens them into `DIR`, with `DIR/.takaro/references.json` written last. `DIR` is per
fingerprint, so a re-pinned target never compiles against the previous build's assemblies.

- A directory recorded for another fingerprint exits 7 (stale reference cache) unless `--force`.
  With `--reuse-by-manifests`, a directory recorded for the same depot manifests and file selection
  is accepted as `up-to-date` whatever else in the target changed; a game names such a directory
  after its references key rather than its fingerprint (Rust does).
- A directory whose files no longer hash as recorded exits 5.
- An unchanged directory is `up-to-date` and nothing is downloaded.

## Exit codes

| Code | When |
|---|---|
| 2 | The lock is missing or malformed, or a credential environment variable the target names is unset. |
| 4 | Steam will not serve a pinned manifest (purged, no licence, login refused). The message always says **not falling back to branch head**. An anonymous 401 or "no manifest request code" for a superseded manifest also sets `"reason": "manifest-not-served"` in the JSON document. |
| 5 | Bytes that are not what the catalog pins: the tool archive, a downloaded file, a reference assembly. |
| 7 | A stale reference directory, a rollback with no previous install, or an artifact that escapes its folder. |

## What this never does

- Install the branch head when a manifest is pinned, or when the pinned one is gone.
- Run an in-place updater (`app_update`, LinuxGSM auto-install) against a pinned install.
- Upload game files anywhere. The cache is local; the release carries the connector only.
- Put a credential in the catalog, a log or the command's output.
