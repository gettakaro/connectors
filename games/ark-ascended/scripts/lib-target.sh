#!/usr/bin/env bash
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"
asa_repo_root() { takaro_repo_root; }
asa_resolve_target() { takaro_resolve_target ark-ascended ARK_ASCENDED "${1:-}"; }
asa_parse_target_flag() { takaro_parse_target_flag "$@"; }
