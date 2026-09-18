# Fabric provider fixtures

Recorded 2026-09-17 with the identifying User-Agent the tool sends
(`takaro-connectors-maint/<version> (+https://github.com/gettakaro/connectors)`):

```
UA="takaro-connectors-maint/0.1.0 (+https://github.com/gettakaro/connectors)"
curl -sS -A "$UA" https://meta.fabricmc.net/v2/versions/game            > versions_game.json
curl -sS -A "$UA" https://meta.fabricmc.net/v2/versions/loader          > versions_loader.json
curl -sS -A "$UA" https://maven.fabricmc.net/net/fabricmc/fabric-api/fabric-api/maven-metadata.xml \
    > fabric-api-maven-metadata.xml
for v in 0.160.7+26.3 0.160.0+26.2; do
  curl -sS -A "$UA" "https://maven.fabricmc.net/net/fabricmc/fabric-api/fabric-api/$v/fabric-api-$v.jar.sha256" \
      > "sha256/$v.sha256"
done
```

- `versions_game.json` — 527 live entries trimmed to the newest 20 plus the release entries
  for 26.1.2, 26.1.1, 26.1 and 1.21.11, in the upstream order (newest first). `stable: true`
  marks a release; every preview keeps Mojang's id verbatim (`26.3-rc-3`, `26.3-pre-3`,
  `26.3-snapshot-10`).
- `versions_loader.json` — the newest 3 of 253 loaders. The listing is identical for every
  game version, so readiness never reads it; the provider records the newest stable loader
  as an informational fact only.
- `fabric-api-maven-metadata.xml` — 1157 live `<version>` entries trimmed to the 122 whose
  build metadata is a `+26.` bucket, in publish order (oldest first). The order is what the
  provider reads: `0.155.3+26.1.2` really is published after `0.159.4+26.3` upstream, so the
  fixture proves the head is the last listed member of a bucket and never the numerically
  largest. `<latest>`, `<release>` and `<lastUpdated>` are the live values and are unread.
- `sha256/<version>.sha256` — the live digest sidecars, plain hex, newline terminated.

Recorded heads: game `26.3` (stable), fabric-api `0.160.7+26.3`, loader `0.19.5`.
