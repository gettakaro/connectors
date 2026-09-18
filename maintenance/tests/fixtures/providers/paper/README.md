# Paper provider fixtures

Recorded 2026-09-17 with the identifying User-Agent PaperMC's download policy requires
(`takaro-connectors-maint/<version> (+https://github.com/gettakaro/connectors)`):

```
UA="takaro-connectors-maint/0.1.0 (+https://github.com/gettakaro/connectors)"
curl -sS -A "$UA" https://fill.papermc.io/v3/projects/paper > projects_paper.json
for v in 26.3 26.2 1.21.11 26.3-rc-3; do
  curl -sS -A "$UA" "https://fill.papermc.io/v3/projects/paper/versions/$v/builds" > "builds/$v.json"
done
```

- `projects_paper.json` — the live document trimmed to the families `26.3`, `26.2` and
  `1.21`, keeping the versions the builds fixtures answer for. Families are newest first and
  so are the versions inside one, which is the order the provider's `window` walks.
- `builds/26.3.json` — the newest 3 of 14 builds, all `ALPHA` (no stable Paper for 26.3 yet).
- `builds/26.2.json` — the newest `STABLE` build (124) and the newest non-stable one (82,
  `BETA`), out of 109.
- `builds/1.21.11.json` — builds 132 and 131, both `STABLE`, out of 92.
- `builds/26.3-rc-3.json` — the single `ALPHA` build of the release candidate.

`commits` is dropped from every build; `id`, `time`, `channel` and
`downloads["server:default"]` (name, url, size and the sha256 checksum) are what the
provider reads. `/v3/projects/paper/versions/<v>` is deliberately not recorded: it returns
build ids without a channel or a time, which is not enough to pick a build.

Recorded heads: `26.2-124` (STABLE), `1.21.11-132` (STABLE), `26.3-16` (ALPHA).
