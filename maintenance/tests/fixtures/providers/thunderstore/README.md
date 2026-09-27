# `GET /api/experimental/package/denikson/BepInExPack_Valheim/` (BepInExPack, the Valheim mod loader)

Recorded 2026-09-27, unauthenticated:

```
curl -fsSL https://thunderstore.io/api/experimental/package/denikson/BepInExPack_Valheim/ \
  | jq '{namespace, name, full_name, date_created, date_updated,
         latest: {namespace, name, version_number, full_name, download_url, date_created, is_active}}'
```

`bepinexpack-valheim.json` keeps only the fields the provider reads. Everything else (the
package's rating and download counters, its categories, the owner's profile) is cut.

This package is in the fixture because of what the endpoint does *not* say. It answers with
the package and its `latest` version and nothing more: there is no list of versions, so a
scan can only ever observe the current head. That is why the provider reports
`history="heads-only"` and attaches its `observationLimit` to every observation, and why a
version published and superseded between two scans is simply never seen. The endpoint also
publishes no digest for the zip, so a pinned `sha256` is always self-recorded by whoever
pinned it.

What Thunderstore served that day: `latest` = `5.4.2351`, created 2026-09-24, downloadable
from `…/package/download/denikson/BepInExPack_Valheim/5.4.2351/`.
