# 1. Dependency updates do not drive releases, and runtime libraries are not automerged

Date: 2026-05-29

## Status

Accepted

## Context

Renovate manages dependency updates across this monorepo. Two properties of the repo
shape how it must be configured:

1. **release-please reads squash-merge commit titles** to decide per-connector version
   bumps and changelog entries. Renovate's default convention commits runtime-dependency
   updates as `fix(deps): ...`, which release-please treats as a release-worthy change.
2. **Per-connector CI is compile/format-only** — `rust` compiles the plugin and builds its
   Docker image, `minecraft` runs a Gradle build, `7d2d` runs `csharpier --check` plus a
   build. There are no runtime or integration tests. A green check proves the connector
   *compiles*, not that it still *works* against a game server.

Nothing in the repo auto-publishes: a Stable release only happens when a human merges a
connector's Release PR. Dependency updates merged to `main` are immediately picked up by
the rolling **dev build** / **dev channel**.

## Decision

**All Renovate commits use the `chore` type** (`:semanticCommitTypeAll(chore)`). Dependency
updates therefore never propose a version bump and never appear in a connector changelog.
They land on `main`, flow into the dev channel, and ride into the next Stable release that a
human-authored `feat`/`fix` triggers.

**Automerge is restricted to updates a compile/format/build check can fully validate**:
GitHub Actions, Docker base-image digests, formatter/build tooling, the Gradle wrapper, the
minecraft bot's devDependencies, and lockfile maintenance. **Runtime libraries** (`gson`,
`log4j`, `java-websocket`, `mineflayer`, `express`, NuGet runtime packages), **all major
updates**, and the **game-coupled minecraft platform group** always require human review.

## Consequences

- Changelogs stay focused on user-facing connector changes; "now supports Minecraft X" is
  authored by the human who bumps the game-coupled set, not buried in a `chore(deps)` line.
- A connector with no recent `feat`/`fix` can carry merged dependency updates on the dev
  channel without a Stable release. This is acceptable for game-server plugins; merging the
  Release PR manually flushes them when needed.
- The blast radius of a bad automerge is "dev channel until the next human-merged Release
  PR," never a broken production release — which is why aggressive automerge of
  CI-vouched-for updates is safe, and why runtime libraries (whose behaviour CI cannot
  vouch for) are deliberately excluded.
- **Future enabler:** wiring a smoke-level runtime test into CI would let us widen automerge
  to runtime libraries with confidence; revisit this decision if that lands.
