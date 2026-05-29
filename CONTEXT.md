# Context

Glossary of domain terms for the Takaro connectors monorepo. Keep this a glossary —
no implementation details.

## Terms

**Connector** — A self-contained plugin that connects one game server to Takaro via the
Generic Connector Protocol. There are three: `rust`, `minecraft`, and `7d2d`. Each is
versioned and released independently.

**Stable release** — A published, semver-versioned build of a single connector
(`<connector>-vX.Y.Z`), intended for production use. Created by merging that connector's
Release PR.

**Release PR** — An automatically maintained pull request, one per connector, that
accumulates the pending changes since the last stable release and proposes the next
version and changelog. Merging it produces the stable release. Nothing is released until
it is merged.

**Dev build** — A bleeding-edge build of a connector produced from the latest `main`,
ahead of the next stable release. Not intended for production.

**Dev channel** — The single, continuously-updated location where a connector's current
dev build is published (`<connector>-dev`). It always points at the most recent dev build;
previous dev builds are not retained.

**Dev version** — The version a dev build reports: the last released version with a
`-dev.<short-commit>` suffix (e.g. `0.0.2-dev.30baac9`). Sorts below the corresponding
stable release.
