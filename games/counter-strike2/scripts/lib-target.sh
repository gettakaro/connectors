#!/usr/bin/env bash
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"
cs2_resolve_target() { takaro_resolve_target counter-strike2 CS2 "${1:-}"; }
cs2_parse_target_flag() { takaro_parse_target_flag "$@"; }
