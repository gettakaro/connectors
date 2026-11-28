#!/usr/bin/env bash
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"
dayz_repo_root() { takaro_repo_root; }
dayz_resolve_target() { takaro_resolve_target dayz DAYZ "${1:-}"; }
dayz_parse_target_flag() { takaro_parse_target_flag "$@"; }
