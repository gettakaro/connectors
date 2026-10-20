# Team Fortress 2 connector: development

A SourceMod plugin (`takaro_tf2.smx`) inside the TF2 dedicated server (srcds). The ProjectSky
[`sm-ext-websocket`](https://github.com/ProjectSky/sm-ext-websocket) and
[`sm-ext-json`](https://github.com/ProjectSky/sm-ext-json) extensions give it an outbound WebSocket
and JSON, so the plugin holds the Takaro connection itself: no sidecar, no inbound port, no RCON.
Those two extensions are production dependencies and ship in the release zip; MetaMod:Source 1.12
and SourceMod 1.13 are installed by the operator.

## Layout

| Path | What it is |
|---|---|
| `mod/addons/sourcemod/scripting/takaro_tf2.sp` | The plugin: Takaro protocol (v0 and v1 with event acks), game adapter, cvars. |
| `mod/addons/sourcemod/scripting/takaro_tf2_item_catalog.inc` | Generated weapon catalog for `listItems` (item definition index, in-game name). |
| `mod/cfg/sourcemod/takaro_tf2.cfg` | The config template shipped in the zip (no secrets). |
| `scripts/build-release.sh` | Release build for one catalog target. |
| `scripts/generate-tf2-item-catalog.mjs` | Regenerates the item catalog from the TF2 item schema. |
| `scripts/run-live-smoke.sh` | Boots a local srcds against the fake Takaro in `tests/live-harness.mjs`. |
| `tests/validate-static.mjs` | Source-level contract checks (actions, events, cvars, wire shapes, `capabilities.json`). |
| `capabilities.json` | What each Takaro action and event does on TF2. |

The zip mirrors the server's `tf/` folder (`addons/sourcemod/...` and `cfg/sourcemod/...`).

## Build

```bash
games/tf2/scripts/build-release.sh 0.1.0 dist --target linux-25995664
games/tf2/scripts/build-release.sh 0.1.0 dist --target windows-25995664
# or through the maintenance tool, as CI does:
maintenance/bin/takaro-maint build --game tf2 --target linux-25995664 --version 0.1.0 --out dist --toolchain container
```

Everything the build downloads is pinned in `catalog/tf2/targets/<platform>-<build>.json`: the
SourceMod 1.13 Linux package (for `spcomp64` and the includes), the extension zips and their licenses,
each by SHA-256, cached under `$TAKARO_MAINT_CACHE/tf2`. The plugin is compiled once, inside the pinned
Debian image, against the Linux extension includes, with zero warnings allowed; the Windows and Linux
zips carry the same `.smx` and differ only in the extension binaries. spcomp stamps the wall-clock
compile time into the plugin, so the build sets it to the commit time; two builds of one commit are
byte-identical (CI checks this). Set `SPCOMP` to compile with a local compiler while developing.

The release version is written into `PLUGIN_VERSION` in the staged copy; release-please also keeps
the tracked `#define PLUGIN_VERSION` line current.

## Tests

```bash
node games/tf2/tests/validate-static.mjs
```

`run-live-smoke.sh` needs a local TF2 dedicated server with MetaMod:Source 1.12 and SourceMod 1.13
installed, and the `ws` npm package for the fake Takaro:

```bash
npm install --prefix games/tf2/_data/harness ws@8
TF2_SERVER_ROOT=/path/to/tf2-server \
TF2_PACKAGE=games/tf2/_data/dist/<fp16>/takaro-tf2-linux-25995664-0.1.0.zip \
  games/tf2/scripts/run-live-smoke.sh
```

It unpacks the zip into the server, points the plugin at the local harness, starts srcds on
`ctf_2fort` and fails unless the plugin identifies, answers every non-destructive action with the
right shape, refuses a command outside the allowlist and a private message to an offline player,
keeps shutdown disabled, and emits bot-backed
connect, death and disconnect events. Your own `takaro_tf2.cfg` is restored afterwards. Logs go to
`games/tf2/_data/logs/`.

## Item catalog

```bash
node games/tf2/scripts/generate-tf2-item-catalog.mjs
```

reads the localized TF2 item schema from
[`tsuza/tf2-item-schema`](https://github.com/tsuza/tf2-item-schema) and writes every `tf_weapon*`
item (code = item definition index) to `takaro_tf2_item_catalog.inc`.

## Behaviour notes

- Identity is the SteamID64 (`platformId` `steam:<id>`). A server started with `-insecure` has no
  Steam auth; real clients then get a `SOURCE_<userid>` fallback id that `banPlayer` refuses.
- `banPlayer` and `unbanPlayer` go through the engine's `banid` and `removeid`; the connector keeps
  its own ledger in `addons/sourcemod/data/takaro_tf2_bans.txt` (reason, expiry) and re-applies it on
  load. `listBans` also reports permanent bans from `cfg/banned_user.cfg`.
- `executeConsoleCommand` runs only commands in `takaro_tf2_command_allowlist_exact` or starting
  with a prefix in `takaro_tf2_command_allowlist_prefixes`. `shutdown` (and the `server.shutdown`
  capability on protocol v1) exists only when `takaro_tf2_shutdown_enabled` is `1`.
- `getPlayerInventory` lists the equipped weapons; `giveItem` answers a structured unsupported error.
- `listLocations` reads the running map (intelligence, team spawns, control points); `getMapInfo`
  answers a disabled map and `getMapTile` a transparent placeholder.
- `entity-killed` covers TF2's `object_destroyed` (Engineer buildings), not every entity removal.

## Release

release-please (`games/tf2`, component `tf2`) opens the release PR; `.github/workflows/tf2.yml`
runs the static checks and hands the build to the shared `connector-release.yml`, which builds every
`candidate`/`maintained` target in `catalog/tf2/targets/`, builds it again to prove identical bytes,
and publishes `takaro-tf2-<platform>-<build>-<version>.zip` plus `SHA256SUMS`. A new TF2 build is a
new target record (`maintenance/bin/takaro-maint steam pin --game tf2 ...`).
