#!/usr/bin/env bash
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"
tf2_repo_root() { takaro_repo_root; }
tf2_resolve_target() { takaro_resolve_target tf2 TF2 "${1:-}"; }
tf2_parse_target_flag() { takaro_parse_target_flag "$@"; }
