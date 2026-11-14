#!/usr/bin/env bash
# The pull-request entry to setup-environment.sh: a Rust update must not turn every unrelated
# pull request red.
#
# Steam serves only the current public manifest to anonymous accounts, so once Rust updates, a
# cold reference cache cannot be filled until a new target lands. When that is what happened and
# this pull request changes no Rust file, the steps that need the assemblies are skipped with a
# warning. Anything else fails with the original exit code: another download failure, a pull
# request that does change Rust, and a refusal of a manifest that is still the public head.
#
# Writes to $GITHUB_OUTPUT:
#   skip=true             the assemblies are not there; skip the steps that need them
#   pin_unavailable=true  the public build has moved past the pin and this pull request changes
#                         no Rust file, so the release legs have nothing they could fetch
#
# Usage: ci-fetch-references.sh [--target <catalog target id>]
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

rust_parse_target_flag "$@"
rust_resolve_target "${TARGET}"

# The tests replace these; nothing else sets them.
SETUP="${RUST_SETUP_ENVIRONMENT:-${SCRIPT_DIR}/setup-environment.sh}"
STEAM_INFO_URL="${STEAM_INFO_URL:-https://api.steamcmd.net/v1/info}"

put_output() {
    if [ -n "${GITHUB_OUTPUT:-}" ]; then
        echo "$1" >> "${GITHUB_OUTPUT}"
    fi
}

changed_paths() {
    if [ -n "${RUST_CI_CHANGED_PATHS:-}" ]; then
        cat "${RUST_CI_CHANGED_PATHS}"
    else
        # --no-renames: a file moved out of games/rust must still count as changing Rust.
        git -C "${SCRIPT_DIR}" diff --name-only --no-renames HEAD^1 HEAD
    fi
}

# A pull request whose changes cannot be read counts as changing Rust: the safe answer is to fail.
touches_rust() {
    local changed
    changed="$(changed_paths)" || return 0
    grep -qE '^(games/rust|catalog/rust)/' <<<"${changed}"
}

# Empty when the lookup fails. The build id is only ever used to name the newer build and to
# refuse a skip when Steam is refusing its own public head.
public_build() {
    curl -fsS --max-time 20 "${STEAM_INFO_URL}/${RUST_STEAM_APP}" 2>/dev/null \
        | jq -r --arg app "${RUST_STEAM_APP}" --arg branch "${RUST_STEAM_BRANCH}" \
            '.data[$app].depots.branches[$branch].buildid // empty' 2>/dev/null \
        || true
}

PUBLIC="$(public_build)"
[[ "${PUBLIC}" =~ ^[0-9]+$ ]] || PUBLIC=""
PUBLIC_NAME="${PUBLIC:-unknown (the Steam app-info lookup failed)}"
# Strictly newer: a lookup that lags behind a freshly landed pin is not a moved head.
STALE=false
if [ -n "${PUBLIC}" ] && [ "${PUBLIC}" -gt "${RUST_STEAM_BUILDID}" ]; then
    STALE=true
fi
RUST_PR=false
if touches_rust; then
    RUST_PR=true
fi

LOG="$(mktemp)"
trap 'rm -f "${LOG}"' EXIT
code=0
"${SETUP}" --target "${RUST_TARGET}" 2>&1 | tee "${LOG}" || code="${PIPESTATUS[0]}"

if [ "${code}" -eq 0 ]; then
    put_output "skip=false"
    if [ "${STALE}" = true ] && [ "${RUST_PR}" = false ]; then
        echo "::notice::${RUST_TARGET} (Steam build ${RUST_STEAM_BUILDID}) is behind the public build ${PUBLIC}. The reference checks ran from the cache; the release build and verify legs are skipped because this pull request changes no Rust file."
        put_output "pin_unavailable=true"
    else
        put_output "pin_unavailable=false"
    fi
    exit 0
fi

# Exit 4 is every upstream failure; only the reason says it was this one.
if [ "${code}" -ne 4 ] || ! grep -q '"reason": "manifest-not-served"' "${LOG}" || [ "${RUST_PR}" = true ]; then
    exit "${code}"
fi
if [ "${PUBLIC}" = "${RUST_STEAM_BUILDID}" ]; then
    echo "::error::Steam refused the manifest of ${RUST_TARGET}, but build ${PUBLIC} is still the public head, so this is not a moved head."
    exit "${code}"
fi

echo "::warning::Skipping the Rust reference checks: ${RUST_TARGET} (Steam build ${RUST_STEAM_BUILDID}) is no longer served by Steam and its reference assemblies are not cached. The newer public build is ${PUBLIC_NAME}. This pull request changes no Rust file; a new Rust target is needed."
put_output "skip=true"
put_output "pin_unavailable=true"
