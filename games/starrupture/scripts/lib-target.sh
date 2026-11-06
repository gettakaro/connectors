#!/usr/bin/env bash
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"
starrupture_repo_root() { takaro_repo_root; }
starrupture_resolve_target() { takaro_resolve_target starrupture STARRUPTURE "${1:-}"; }
starrupture_parse_target_flag() { takaro_parse_target_flag "$@"; }
