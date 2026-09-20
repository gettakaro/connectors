#!/usr/bin/env bash
# Fails a new game from shipping without website-facing metadata:
#   - games/<game>/connector.json exists and validates against games/connector.schema.json
#   - games/<game>/README.md satisfies games/README-contract.md
# The README is published as https://takaro.io/docs/supported-games/official/<game>, and that
# sync is all-or-nothing across all ten games, so one malformed README blocks every game's docs
# refresh. Catching it here means it fails in the repo that broke it.
# Usable locally or in CI (needs npx/node).
set -euo pipefail
cd "$(dirname "$0")/.."

SCHEMA="games/connector.schema.json"
GAMES=$(jq -er '.packages | keys[] | ltrimstr("games/")' release-please-config.json)

FAILED=0
for game in $GAMES; do
  METADATA="games/${game}/connector.json"
  if [ ! -f "$METADATA" ]; then
    echo "Error: ${game} is missing ${METADATA}" >&2
    FAILED=1
    continue
  fi
  if ! npx --yes ajv-cli@5 validate -s "$SCHEMA" -d "$METADATA" --spec=draft2020 --strict=true >/dev/null; then
    echo "Error: ${METADATA} does not validate against ${SCHEMA}" >&2
    FAILED=1
  fi

  README="games/${game}/README.md"
  if [ ! -f "$README" ]; then
    echo "Error: ${game} is missing ${README}" >&2
    FAILED=1
    continue
  fi

  # Contract rule 1. The sync turns this line into the page title, so its shape is not cosmetic.
  if ! head -n 1 "$README" | grep -qE '^# Takaro .+ Connector$'; then
    echo "Error: ${README} line 1 must be '# Takaro <Game> Connector' (see games/README-contract.md)" >&2
    FAILED=1
  fi
  # One H1 only: a second one would render as a duplicate title inside the published page.
  # Counted outside fenced code blocks — every install section has a `# Carbon`-style shell
  # comment in a fence, and a naive grep reads those as headings and fails a correct README.
  H1S=$(awk '/^```/ { fence = !fence; next } !fence && /^# / { n++ } END { print n + 0 }' "$README")
  if [ "$H1S" -ne 1 ]; then
    echo "Error: ${README} has ${H1S} H1 headings, expected exactly 1" >&2
    FAILED=1
  fi
  # Contract rule 3.
  if ! grep -qE '^## Install$' "$README"; then
    echo "Error: ${README} has no '## Install' section (see games/README-contract.md)" >&2
    FAILED=1
  fi
  # Contract rule 4, both halves: the heading and an actual legend under it. A capability section
  # with no verdict markers is a section nobody filled in.
  if ! grep -qE "^## What works, what doesn't$" "$README"; then
    echo "Error: ${README} has no \"## What works, what doesn't\" section (see games/README-contract.md)" >&2
    FAILED=1
  elif ! sed -n "/^## What works, what doesn't\$/,\$p" "$README" | grep -qE '✅|⚠️|❌'; then
    echo "Error: ${README} \"What works, what doesn't\" uses none of the ✅/⚠️/❌ legend" >&2
    FAILED=1
  fi
  # Contract rule 6. The sync has no asset pipeline, so an image is a guaranteed broken link.
  if grep -qE '!\[[^]]*\]\(' "$README"; then
    echo "Error: ${README} embeds an image; takaro.io has no asset pipeline for READMEs" >&2
    FAILED=1
  fi
done

if [ "$FAILED" -ne 0 ]; then
  echo "connector metadata validation failed" >&2
  exit 1
fi

echo "All connector.json files and READMEs are present and valid."
