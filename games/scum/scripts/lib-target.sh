#!/usr/bin/env bash
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"
scum_repo_root() { takaro_repo_root; }
scum_resolve_target() { takaro_resolve_target scum SCUM "${1:-}"; }
scum_parse_target_flag() { takaro_parse_target_flag "$@"; }
