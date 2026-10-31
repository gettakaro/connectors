# Connector README template

Every `games/<game>/README.md` uses exactly this layout, in this order, with these headings. It
is published as the game's install guide on takaro.io, so it is written for a server owner who
has a game server and a Takaro account and nothing else. See [README-contract.md](README-contract.md)
for what CI checks.

````markdown
# Takaro <Game> Connector

<One sentence: what it is (plugin / mod / native library) and which server OS.> Players do not
install anything.

## Install

You need: <server type and OS, framework or mod loader if any, a Takaro game server of type
Generic and its registration token>.

### 1. Download

From https://takaro.io/connectors/<id>, download `<exact file name>`. <Which file for which
server build, in one line or the generated target table. Use `<version>`, never a fixed version.>

### 2. Install

<Where the files go: one code block with the path, plus a Linux / Windows difference if any.>

### 3. Configure

<Where the config file is, a minimal config example with the token, and whether a restart is
needed.>

<Optional settings, only those owners really change, as a short table. The server chat name is
one row.>

### 4. Check that it works

<The one log line that means connected, and "the server shows as online in Takaro". One line on
what a wrong token looks like.>

**Upgrading:** <one line, only when a manual step is needed.>

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
- **About 350 to 600 words.** The install steps matter most. No explanations of why something
  works the way it does, no options an owner does not need, no environment-variable lists (one
  line at most), no file-name history and no developer notes (those go in DEVELOPMENT.md).
- **Notes are one short phrase**: what the server owner gets or what the limit is. An empty note
  is fine for a plain ✅.
- **No test or run narrative.** No "proven live", "verified with a real game client", "re-proven
  on", "no client in that run", event ids, PR or issue numbers, rig or staging names, counts from
  a test session, "during testing", or the history of how a feature came to work. The one line
  under "What works" is the only place a test date appears.
- **No version history.** What changed between versions belongs in CHANGELOG.md and
  UPGRADING.md. The README describes the latest release only (a manual upgrade step may stay as
  the one "Upgrading" line).
- **No Takaro-internal talk** a server owner cannot act on: "protocol version 1", "declares
  `locations.list`", "Takaro-side echo". Say what the owner sees instead.
- **One download place**: https://takaro.io/connectors/<id>. Mention the GitHub releases page at
  most once, as the alternative. No SHA256SUMS or legacy file names.
- Keep generated blocks (`<!-- takaro-maint:targets:begin -->` … `end -->`) byte for byte.
- Plain, short sentences. No marketing.
