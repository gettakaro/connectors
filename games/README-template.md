# Connector README template

Every `games/<game>/README.md` uses exactly this layout, in this order, with these headings. It
is published as the game's install guide on takaro.io, so it is written for a server owner who
has a game server and a Takaro account and nothing else. See [README-contract.md](README-contract.md)
for what CI checks.

````markdown
# Takaro <Game> Connector

<Two or three sentences: what it is (plugin / mod / native library), which server OS, and
"Players do not install anything." (or exactly what they install, if they must).>

## Install

### 1. Before you start

- <bullets: server type and OS, framework/loader if any, access needed, a Takaro game server of
  type Generic and its registration token>

### 2. Download

Download the latest release from https://takaro.io/connectors/<id>. <Which file for which server
build, as a short list or the generated target table. One sentence on SHA256SUMS. Never name a
fixed version number in an example file name: use `<version>`.>

### 3. Install

<Where the files go: a code block with the folder tree, then Linux and Windows example paths.>

### 4. Configure

<Where the config file is, the minimal config, where the token goes, whether a restart is needed.>

### 5. Check that it works

<The log line(s) to look for, and "the game server shows as online in Takaro". What a wrong or
missing token looks like.>

### 6. Upgrade

<Stop, replace which files, keep which files, start. Link UPGRADING.md / MIGRATION.md if any.>

## What works, what doesn't

Checked on <game version / server build>, <YYYY-MM-DD>. ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | | |
| Player list | | |
| Player location | | |
| Teleport | | |
| Player inventory | | |
| Item list | | |
| Entity list | | |
| Locations | | |
| World map | | |
| Send a message | | |
| Console commands | | |
| Kick | | |
| Ban and unban | | |
| Ban list | | |
| Give an item | | |
| Shop | | |
| Shut down the server | | |
| Player joined / left | | |
| Chat messages | | |
| Player death | | |
| Entity killed | | |
| Server log lines | | |
| Discord chat bridge | | |
| Modules | | |

## Known issues

- <only real, current limits a server owner can hit; at most about five bullets; omit the
  section when there are none>

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
````

## Rules

- **Same headings, same order, same table rows** for every game. Rows keep the names above and
  the order above. After them, only where they apply and in this order: "Game name, version and
  icon", "Restarts and outages", "Game updates". A game-specific capability goes last (for example
  "PrismaCore commands"). Economy belongs in the Shop row. A capability the game does not have stays in the table as ❌
  with a short reason.
- **Notes are one short sentence** (two at most): what the server owner gets or what the limit is.
  An empty note is fine for a plain ✅.
- **No test or run narrative.** No "proven live", "verified with a real game client", "re-proven
  on", "no client in that run", event ids, PR or issue numbers, rig or staging names, counts from
  a test session, "during testing", or the history of how a feature came to work. The one line
  under "What works" is the only place a test date appears.
- **No version history.** What changed between versions belongs in CHANGELOG.md and
  UPGRADING.md. The README describes the latest release only (an upgrade step from an older layout
  may stay in "6. Upgrade" in one or two sentences).
- **No Takaro-internal talk** a server owner cannot act on: "protocol version 1", "declares
  `locations.list`", "Takaro-side echo". Say what the owner sees instead.
- **One download place**: https://takaro.io/connectors/<id>. Mention the GitHub releases page at
  most once, as the alternative.
- Keep generated blocks (`<!-- takaro-maint:targets:begin -->` … `end -->`) byte for byte.
- Plain, short sentences. No marketing.
