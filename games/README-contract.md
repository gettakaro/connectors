# Connector README contract

`games/<game>/README.md` is **published**. It is the source of
`https://takaro.io/docs/supported-games/official/<game>`, synced by the website's
`just seed-connector-docs` verb and refreshed automatically whenever this repo publishes a release.
The page on takaro.io is generated — nobody edits it there — so a server owner reads what is
written here.

That is the whole reason for this document: a README that drifts from the contract below does not
produce a worse page, it fails the sync and blocks the docs refresh for **all ten games**. The sync
is all-or-nothing on purpose (a partial docs update is worse than a stale one), so
`scripts/check-connector-metadata.sh` enforces the structural parts of this contract in CI. A new
game fails lint here rather than breaking the website later.

## What a README must contain

### 1. One H1, of the form `Takaro <Game> Connector`

The first line is `# Takaro <Game> Connector`. The sync strips it and turns it into the page's
front-matter title, so a README with two H1s, no H1, or an H1 in some other shape produces a page
with the wrong title or no title at all.

The `<Game>` part is free text — `RuneScape: Dragonwilds` and `Project Zomboid` are both fine. It
is the `Takaro … Connector` frame that is fixed.

### 2. A lead paragraph

Immediately after the H1, before the first `##`: what the connector is, what it is built against,
and — the question every server owner asks first — **whether players have to install anything**.
Say "players do not install anything" explicitly when that is true.

### 3. An `## Install` section

Numbered `###` subsections, self-contained enough that a server owner never has to open this
repository to follow them. Assume they have a game server and a Takaro account and nothing else.
Include where files go, how the connector is configured, and how to tell that it worked.

### 4. A `## What works, what doesn't` section

A table of capabilities using the ✅ / ⚠️ / ❌ legend, stating plainly what has been verified
against a live server and what has only been read off the source. Name the exact game build it was
tested against, and date the claim. An honest ⚠️ is worth more than an optimistic ✅ — this table
is the one place a server owner finds out what they are actually getting.

### 5. Links

- **External URLs**: absolute, as normal.
- **Links to takaro.io**: write them absolute (`https://takaro.io/connectors/rust`). The sync
  rewrites them to root-relative (`/connectors/rust`) so the published page does not make a
  round trip out to the public site and back.
- **Links to sibling files in this repo** (`DEVELOPMENT.md`, `COMPANION.md`): write them relative,
  as you would for a reader on GitHub. The sync rewrites them to GitHub blob URLs, so they keep
  working on the published page.

### 6. No images

The sync has no asset pipeline: an image in a README is a link to a file that does not exist on
takaro.io. Use a fenced code block for directory trees and console output instead — which is what
every README here already does.

## What the CI check enforces

`scripts/check-connector-metadata.sh` asserts 1, 3, 4 and 6 structurally for every game in
`release-please-config.json`, alongside the `connector.json` validation it already did. 2 and 5 are
judgement calls and are left to review.
