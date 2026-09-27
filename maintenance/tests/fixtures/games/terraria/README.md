# `GET /repos/Pryaxis/TShock/releases` (TShock, the Terraria server framework)

Recorded 2026-09-27, unauthenticated, with the tool's own user agent:

```
curl -A "takaro-connectors-maint/0.1.0 (+https://github.com/gettakaro/connectors)" \
  "https://api.github.com/repos/Pryaxis/TShock/releases?per_page=10" > tshock-releases.json
```

`tshock-releases.json` keeps only the fields the provider reads — `tag_name`, `name`,
`prerelease`, `draft`, `published_at`, and per asset `name`, `size`, `digest`, `updated_at`,
`browser_download_url` — and only the Linux assets. Everything else (author, uploader, body,
reactions, the API's own URLs, the Windows and macOS assets) is cut.

Four releases are kept, and each is here for a reason:

- **`v6.2.1`** — the release the catalog pins. Its name, `TShock 6.2.1 for Terraria 1.4.5.8`, is
  where the Terraria version comes from: nothing in the release payload states it as a field, and
  no provider opens a DLL to find out.
- **`v6.1.0`** — the previous stable one, so "newest" is an ordering and not the only entry.
- **`v6.0.0-pre3`** — a prerelease. The `release` channel must not observe it.
- **`v5.2.4`** — an older release, and the awkward one twice over. Its assets carry
  `"digest": null` (GitHub only started returning asset digests later), so the provider has to
  handle a release it cannot hash without inventing one. And its Linux asset is named
  `…-linux-amd64-Release.zip`, not `-linux-x64-`, which is why the watch's asset pattern accepts
  both spellings.
