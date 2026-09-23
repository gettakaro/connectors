# Operations

How the one-shot maintenance command is run: on a developer's machine, inside the pinned tool
container, and from the dispatched workflow. All three are the same command with the same
arguments — the difference is only where the toolchain comes from.

## One command, three places

```
# host
maintenance/bin/takaro-maint run --repo gettakaro/connectors

# tool container (the checkout is mounted at /repo)
just maint-container-build
just maint-container run --repo gettakaro/connectors

# CI
gh workflow run maintenance.yml -f mode=read-only -f sources="mojang-meta"
```

`just maint-container` is the same `docker run` the workflow performs — the checkout mounted
at `/repo`, and credentials forwarded by name only:

```
docker run --rm -v "$PWD:/repo" \
    -e GH_TOKEN -e TAKARO_MAINT_REPO -e TAKARO_MAINT_GITHUB_API_URL \
    takaro-maint:local run --repo gettakaro/connectors
```

`-e NAME` without a value forwards the variable only when it is set, and never puts its value
on a command line. The recipe also forwards every
`TAKARO_MAINT_STEAM_BRANCH_PASSWORD__*` it finds in the environment.

`gh workflow run` needs the workflow to exist on the repository's **default branch** — GitHub
registers a `workflow_dispatch` trigger from there and nowhere else. Until `maintenance.yml`
is on `main`, dispatching it from a feature branch answers `HTTP 404`, however the `--ref` is
spelled.

`run` is `scan` and then `reconcile` in one process and one report. **It is read-only unless
`--publish` is given** — without that flag it reads the tracker, prints what it would file, and
writes nothing at all.

The workflow's inputs are nothing but a spelling of the same flags:

| Workflow input | Becomes |
|---|---|
| `mode=publish` | `--publish` (`mode=read-only`, the default, adds no flag) |
| `sources` | one `--source <id>` per space-separated word |
| `game` | `--game <id>` |
| `bootstrap` | `--bootstrap` |
| `repo` | `--repo <owner/name>` (empty: the repository the workflow runs in) |

The output contract is the command's, everywhere:

- **stdout** is exactly one JSON document, `{"schemaVersion": 1, "op": "run", "ok": …, …}`,
  with the scan report, the reconcile report and `exitCodes` for both halves.
- **`--out FILE`** writes the same document to a file with mode `0600`. The workflow passes
  `--out /out/result.json` and keeps it as an artifact.
- **stderr** carries progress and diagnostics; `--quiet` and `--verbose` apply.
- **exit codes** are the command's: `0` ok, `2` usage or catalog invalid, `4` an upstream
  source failed, `9` a tracker/GitHub failure or a missing credential. The full table is in
  [the README](../README.md). The workflow does not interpret them: the `run` step's exit code
  is the job's result, so a nonzero run is a red workflow.

## The container

`maintenance/Dockerfile` builds a **toolchain**, not an application. It holds:

- `python:3.12-slim` pinned by digest (the Debian userland),
- `uv` 0.12.15 pinned by digest, and a uv-managed Python 3.12 installed at build time,
- `steamcmd`, self-updated once during the build, reachable as `steamcmd` on `PATH` (a two-line
  wrapper, not a symlink: `steamcmd.sh` derives its own root from `$0`),
- DepotDownloader, fetched by the command's own lock-checked acquisition against
  `tools.lock.json`,
- the virtual environment from `uv sync --frozen`.

It holds no code of its own beyond the copy of `maintenance/` that the environment was built
from, no catalog it could read instead of yours, and no credential.

**Always mount the checkout at `/repo`.** The entrypoint is the same launcher a host run uses,
`/repo/maintenance/bin/takaro-maint`, and the command resolves the repository root from where
its package sits — so with the mount it reads your catalog, your `uv.lock` and your source.
Without the mount there is no catalog and nothing resolves.

Pass `--repo owner/name` (or `TAKARO_MAINT_REPO`). The fallback — reading the `origin` remote —
needs git to trust the mounted checkout, which it does only when the container's uid matches the
files' owner; that is what `--build-arg UID` / `--build-arg GID` are for, and both `just
maint-container-build` and the workflow pass them.

Rebuild the image when `uv.lock` or `tools.lock.json` changes: the image sets `UV_NO_SYNC=1` so
a run never re-syncs from the mounted project, which is what keeps a run offline-capable and its
stderr free of the package manager's own chatter.

**Re-pinning DepotDownloader.** Its upstream zip has no published checksum, so `tools.lock.json`
records the size and sha256 this repository saw. If upstream replaces the asset, the image build
fails at the `depotdownloader.ensure(...)` layer — by design, rather than shipping unknown bytes.
To re-pin: download the archive twice, confirm both downloads agree on sha256 and size, and
update `sha256`, `size` and `recordedAt` in `maintenance/tools.lock.json`. steamcmd, by contrast,
is published as one unversioned, self-updating tarball; there is nothing to pin, so it is not
pinned.

`maintenance/tests/test_container_parity.sh` is the proof that the container and the host are the
same command: it builds the image and compares `--version`, `targets list`, `catalog validate`
and the credential-less tracker path byte for byte, checks that no layer and no image
environment names a credential, compares the image's sources against the checkout's file by
file, and runs the fixture scenario inside the image with `--network none`. Run it with `bash
maintenance/tests/test_container_parity.sh`; it prints `SKIP: docker missing` and exits 0 where
there is no docker. `maintenance-ci.yml` does not run it — a docker build on every pull request
is disproportionate — so it is a local and release-evidence check.

## Credentials

Credentials come from the environment, never from a flag and never from a file in the
repository. Any value of a variable whose name looks like a credential is replaced with
`<redacted>` in everything the command writes: stdout, stderr, `--out` files and therefore the
workflow artifacts too.

| Variable | Used by | Notes |
|---|---|---|
| `GH_TOKEN` | `scan`, `reconcile`, `run`, `dashboard`, `release` | Required — read-only still reads the tracker. Falls back to `gh auth token`. Never a flag. |
| `TAKARO_MAINT_REPO` | the same | The `--repo` equivalent; otherwise the `origin` remote. Never `GITHUB_REPOSITORY`. |
| `TAKARO_MAINT_GITHUB_API_URL` | the same | The `--api-url` equivalent. |
| `TAKARO_MAINT_STEAM_BRANCH_PASSWORD__<app>__<LABEL>` | the Steam provider | One protected branch's password. Missing means a failed observation, never an empty success. |
| `TAKARO_MAINT_CACHE` | all | The download cache; steamcmd's logs live under `steam/logs/`, DepotDownloader under `tools/`. |
| `TAKARO_MAINT_STEAMCMD`, `TAKARO_MAINT_DEPOTDOWNLOADER`, `TAKARO_MAINT_DOCKER`, `TAKARO_MAINT_GRADLE` | tool overrides | The container sets none of them: steamcmd is on `PATH` and DepotDownloader is in the cache. |

In CI the command only ever sees `GH_TOKEN`. It comes from `actions/create-github-app-token`
where the App's secrets exist, and from the workflow token on a fork or a private mirror. For
`mode=publish` the App needs **Issues: read & write** on the tracker repository; `mode=read-only`
needs no write permission at all. No secret is interpolated into a `run:` script, passed as a
`docker build` argument or baked into a layer — `test_dashboard_show.py` and
`test_container_parity.sh` both check that, from the two sides.

## Dashboard health

```
maintenance/bin/takaro-maint dashboard show
maintenance/bin/takaro-maint dashboard show --format table
```

`dashboard show` is a read: it never writes, and `health` is derived when you ask rather than
stored on the board.

| Field | Meaning |
|---|---|
| `issue`, `url`, `updatedAt` | The dashboard issue, `null` when the tracker has none yet. `url` is only ever a real `github.com` address; against another API host it stays `null` rather than guessing. |
| `lastSuccess` | When a run last finished with every source healthy. |
| `health` | `absent` (no dashboard), `ok` (every listed source `ok` and `lastSuccess` set), otherwise `degraded`. |
| `counts` | How many sources are listed, and how many are `ok`, `failed` or `uninitialized`, plus the number of filed work items. |
| `sources` | Per source: `status`, `history`, the observed `heads`, the checkpoint (`seen` count, `floor`, `at`), `lastSuccess` and `lastError`. |
| `work` | Every piece of work the tracker has filed, by identity: its issue, state and since when. |
| `targets` | The catalog snapshot the last publishing run recorded. |

`--game G` narrows the source list to one game, and then `health` and `counts` describe that
view rather than the whole board — ask without it for the board's own health.

`uninitialized` means the catalog watches that source but no run has ever recorded a checkpoint
for it. Automatic runs (catalog pushes to `main` and the six-hour schedule) pass
`--bootstrap` and initialize new sources without a separate dispatch. For a manual
run, use `run --bootstrap --publish`, which records what is already published and
files only the heads nothing covers. A non-null `lastError` is the last thing that source's
provider said; rerun once (upstream outages are common and transient), and if it persists, fix
the source. `dashboard show` exits 0 whether the board is healthy or degraded: failing a process
because a source failed is `run`'s job.

## Failure semantics and resumption

- **A source failed** → the run exits `4`. That source's checkpoint is kept exactly where it
  was, so the next run retries the whole source; every other source's checkpoint advances and
  their issues stay filed. The per-source outcome is in the report and on the board.
- **A tracker write failed** → the run exits `9` and nothing is half-written. The dashboard is
  written with a compare-and-swap (re-read immediately before the update, refuse if it moved),
  so a concurrent writer makes a run stop rather than overwrite someone's checkpoints.
- **Rerunning is a no-op for what was already filed.** Issues are found by their identity
  marker, not by the checkpoint, so an interrupted run that filed an issue and never recorded it
  finds that same issue on the next run instead of filing a second one.
- **A fresh runner needs no local state.** Every checkpoint and every filed identity lives in
  the dashboard issue; the download cache is the only thing a run leaves on disk, and it is a
  cache. An ephemeral CI runner, a container and a laptop all resume from the same place.
- **Diagnostics are bounded.** A workflow run uploads `maintenance-<run_id>`: `result.json`
  (the redacted report), `stderr.log`, and `steam-logs/**` when a Steam source ran. Retention is
  14 days.

## Schedule

The workflow carries `schedule: - cron: '17 */6 * * *'` — four times a day, at minute 17 — and
`concurrency: {group: maintenance-publisher, cancel-in-progress: false}`, so there is one
publisher at a time and a running one is never cancelled (a cancelled run could leave issues
filed without their checkpoint).

Catalog changes pushed to `main` also trigger an automatic run. Both automatic triggers
initialize new sources with `--bootstrap` and are gated on a tracked file, `maintenance/config/schedule.yaml`:

```yaml
publish_schedule: disabled
```

While it says `disabled`, an automatic run exits 0 after printing a `::notice::` and does nothing
at all. Enabling it is a one-line change to that file, reviewed and merged like any other —
deliberately not a repository setting somebody can flip unseen. Note that GitHub runs `schedule`
events only on the default branch: the cron does not fire from a pull request branch, so the
first scheduled run happens after this lands on `main`.

**Disable CI publication before moving scheduling to a cluster cron.** In order:

1. set `publish_schedule: disabled` and merge it;
2. wait for the next scheduled run and confirm it reports the notice and does nothing;
3. only then start the cluster cron, running the same container with the same environment.

Never leave two publishers running. The compare-and-swap means they cannot corrupt the board —
they make each other fail with exit `9` — but every second run would be wasted work and a red
alert about nothing.

## Out of scope

Deploying this to a cluster, running distributed writers, and any external alerting service are
outside what this repository provides. `dashboard show` is the health surface; what watches it
is somebody else's system.
