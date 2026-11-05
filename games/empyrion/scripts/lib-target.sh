#!/usr/bin/env bash
# The catalog target every Empyrion script builds against.
#
# Source it, do not run it. `empyrion_resolve_target [target-id]` exports the EMPYRION_* keys
# `takaro-maint targets resolve` produces (toolchain image, reference hashes, artifact name), so
# no script here hard-codes a game build, an image digest or a reference assembly.
# shellcheck source=../../../scripts/lib/target.sh
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"
empyrion_repo_root() { takaro_repo_root; }
empyrion_resolve_target() { takaro_resolve_target empyrion EMPYRION "${1:-}"; }
empyrion_parse_target_flag() { takaro_parse_target_flag "$@"; }

# empyrion_references <dest> -- the three server assemblies the mod compiles against
# (Mif.dll, ModApi.dll, Newtonsoft.Json.dll), fetched from the pinned depot manifest by
# `takaro-maint steam references` (the Managed subset only, never the 18 GB depot), then
# checked against the catalog hashes. Needs empyrion_resolve_target first.
empyrion_references() {
    local dest="${1:?empyrion_references <dest>}"
    "${TAKARO_MAINT}" steam references --game empyrion --target "${EMPYRION_TARGET}" --dest "$dest" >/dev/null
    empyrion_check_references "$dest"
}

# empyrion_check_references <dir> -- every pinned reference in <dir> hashes to the catalog value.
# EMPYRION_REFERENCES is "<file>:<sha256>;..." for each file build.references selects.
empyrion_check_references() {
    local dir="${1:?empyrion_check_references <dir>}" entry name sha
    local -a entries
    [ -n "${EMPYRION_REFERENCES:-}" ] || { echo "EMPYRION_REFERENCES is not set; resolve the target first" >&2; return 2; }
    IFS=';' read -r -a entries <<< "${EMPYRION_REFERENCES}"
    for entry in "${entries[@]}"; do
        name="${entry%%:*}"
        sha="${entry#*:}"
        if ! printf '%s  %s\n' "$sha" "${dir}/${name}" | sha256sum --check --status; then
            echo "error: ${dir}/${name} is not the ${name} that ${EMPYRION_TARGET} pins (sha256 ${sha})" >&2
            return 5
        fi
    done
}
