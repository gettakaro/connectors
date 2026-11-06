# Icon Packs

An **Icon Pack** is the set of small item icons for one game, served to Takaro from an icon bucket.
`takaro-maint icons` builds a pack from a recipe document and publishes it. Game binaries and raw
game files are still never uploaded or committed; only the small derived icons (WebP, at most 128px)
go to the icon buckets, and they are not kept in git (see [support-policy.md](support-policy.md)).

## Commands

| Command | What it does |
|---|---|
| `icons build --recipes FILE --out DIR` | Render a recipe document into `DIR/pack/<game>/`, plus a coverage report, a contact sheet and a build record outside the pack. `DIR` must be absent or empty. |
| `icons publish --env staging --pack DIR [--dry-run]` | Merge the pack into the live index and upload it to the staging bucket. `DIR` is `…/pack/<game>`. |

Not built yet: `icons promote`, `icons set-enabled` and `icons takedown` (#810, the only commands
that write production or delete), catalog-derived recipes with `--game/--target/--client` (#744) and
the live item-code list with `--codes` (#811, skipped for now, so coverage is measured against codes
derived from game data and the report says so).

## The recipe document

```json
{
  "schemaVersion": 1,
  "game": "fixture-846",
  "measuredAgainst": "game-data",
  "recipes": [
    {"code": "fixture-circle", "kind": "item", "layers": [{"image": "images/circle-512.png", "ops": []}], "composite": "over"}
  ],
  "unmatched": [
    {"code": "fixture-ghost", "kind": "item", "reason": "no-icon-in-game"}
  ]
}
```

- `game` is a valid game identifier (below). Codes are non-empty, at most 256 characters, without
  control characters, and unique across `recipes` and `unmatched` together.
- `kind` is optional: `item` or `block`. `reason` matches `^[a-z][a-z0-9-]*$` and is an open set.
- `image` is a PNG path relative to the recipe file, checked by `paths.safe_relative`. A missing
  file is exit 4.
- This tracer accepts exactly one layer and no `ops`; image operations and layer compositing arrive
  with the games that need them. `measuredAgainst` can only be `game-data`.

The synthetic fixture is `maintenance/tests/fixtures/icons/fixture-846/`.

## The pack contract

Shared with Takaro, whose reader rejects the whole index when it breaks a rule.

```
<game>/index.json
<game>/<sha256-of-webp-bytes>.webp
```

```json
{"game": "<game>", "enabled": true, "icons": {"<item code>": "<sha256>.webp"}}
```

- Game identifier: `^[a-z0-9][a-z0-9-]*$`, at most 64 characters. `_store/` belongs to Takaro's Icon
  Store (takaro ADR 0051) and is never written, checked or deleted by this tool.
- Objects are WebP, longest edge at most 128px, never upscaled, named by the sha256 of their bytes,
  so identical art is stored once.
- `index.json` has exactly the keys `game`, `enabled`, `icons`; it is serialized by one function
  (`contract.serialize_index`: sorted codes, two-space indent, trailing newline), so build and
  publish agree byte for byte.
- Headers: `.webp` is `image/webp` with `public, max-age=31536000, immutable`; `index.json` is
  `application/json` with `public, max-age=300`.

`contract.check_pack_dir` runs at the end of `icons build` and again at the start of `icons publish`.
It needs the directory to be named for its game, to hold only `index.json` and `<sha256>.webp`
files, every referenced object to exist, every object to be referenced, every hash to match its
name and every image to open as WebP within 128px.

## WebP encoding and the Pillow pin

Rendering is pinned so that the same recipes give the same bytes: open the PNG, convert to RGBA,
copy the pixels into a fresh image (no ICC, EXIF or XMP), downscale a longest edge over 128 with
LANCZOS on premultiplied pixels, then encode lossless (`quality=100, method=6, exact=False`).

The bytes depend on the Pillow wheel and the libwebp it bundles, so `pillow` is pinned exactly in
`pyproject.toml`. Bumping it changes object hashes. That is acceptable, because objects are
content-addressed and the rolling merge repoints codes, but do it deliberately. `build-record.json`
logs both versions.

## Reports

- `coverage.json`: total, matched, unmatched, `coverage` (matched over total), object count,
  `byKind` (only when entries carry a kind), `unmatchedByReason`, and the sorted `unmatchedCodes`
  with their reasons. `measuredAgainst` is `game-data`, with a note saying so.
- `contact-sheet.png`: up to 64 matched icons (an evenly spaced sample beyond that), eight columns,
  on a checkerboard with the code underneath.
- `build-record.json`: the recipe file's sha256, the encoder versions and settings, and the counts.

None of them is inside `pack/`, so none is published.

## Publish protocol

1. Refuse any `--env` other than `staging` (production is written by `icons promote`).
2. Refuse a `_store/` pack, then run the contract check on the local pack.
3. Resolve credentials from the environment. No client exists before this step.
4. `GET <game>/index.json`. A 404 means there is no live index; an invalid one (or one for another
   game) stops the run with exit 7 before anything is uploaded.
5. Merge: every live code survives, a changed code points at the new object, and `enabled` is
   taken from the live index (a takedown is not undone; a new pack is enabled). The merged index is
   validated.
6. `HEAD` every object; existing ones are skipped.
7. In a dry run, stop and print the plan.
8. `PUT` each missing object, then `PUT` `index.json` last, then `GET` it back and compare. An index
   that merges to the live bytes is not uploaded (`unchanged`). A failed object upload stops the
   run with exit 10 and the index is not uploaded; a rerun skips what already landed. Old objects
   are never deleted.

stdout is one JSON document with `objects` (`uploaded` or `wouldUpload`, and `skipped`) and `index`
(`action`, `codes`, `enabled`, `added`, `repointed`, `kept`, `unchanged`). The endpoint, bucket and
keys are never printed.

## Environment

| Variable | Use |
|---|---|
| `STG_R2_ACCESS_KEY_ID`, `STG_R2_SECRET_ACCESS_KEY`, `STG_R2_ENDPOINT`, `STG_R2_BUCKET` | The staging bucket (Cloudflare R2, S3-compatible, path-style). All four are redacted. The endpoint must be `https://` (loopback `http://` is allowed for the test fake). |
| `STG_ICON_HOST` | Public staging host, for curl verification only. No command reads it. |
| `PROD_*` | Reserved for `icons promote`; `icons publish` never reads them. |

## Exit codes

| Code | When |
|---|---|
| 0 | ok, including a dry run and a republish where nothing changed |
| 2 | usage or configuration: a bad flag or recipe document, `--env` other than `staging`, a `_store/` or otherwise invalid key, a missing `STG_R2_*` variable, a non-https endpoint, a non-empty `--out`, an unsupported op or multi-layer recipe |
| 4 | a recipe image file is missing |
| 5 | the index read back after upload differs from what was sent |
| 7 | contract violation in the built pack, the local pack, the live index or the merged index |
| 10 | a bucket read or upload failed (S3 error, unreachable endpoint, access denied) |

## Runbook: publish fixture-846 to staging

From the repository root, with the staging variables in a gitignored `.env` (never print them):

```bash
OUT="$(mktemp -d)/fixture-846"
maintenance/bin/takaro-maint icons build \
  --recipes maintenance/tests/fixtures/icons/fixture-846/recipes.json --out "$OUT"
( set -a; . ./.env; set +a
  maintenance/bin/takaro-maint icons publish --env staging --pack "$OUT/pack/fixture-846" --dry-run )
( set -a; . ./.env; set +a
  maintenance/bin/takaro-maint icons publish --env staging --pack "$OUT/pack/fixture-846" )
```

A second publish must skip all five objects and leave the index `unchanged`. Verify through the
public host:

```bash
curl -fsS https://icons.takaro.dev/fixture-846/index.json | jq .
curl -sSI https://icons.takaro.dev/fixture-846/index.json | grep -iE '^(HTTP|content-type|cache-control)'
cmp <(curl -fsS https://icons.takaro.dev/fixture-846/index.json) "$OUT/pack/fixture-846/index.json"
for f in $(curl -fsS https://icons.takaro.dev/fixture-846/index.json | jq -r '.icons[]' | sort -u); do
  curl -sS -o /dev/null -w "%{http_code} %{content_type} %header{cache-control} $f\n" "https://icons.takaro.dev/fixture-846/$f"
done
```

Expect `200 application/json public, max-age=300` for the index and
`200 image/webp public, max-age=31536000, immutable` for every object. A 404 right after the first
publish can be a cached miss at Cloudflare: retry with `?v=$(date +%s)` and again after a few minutes.
