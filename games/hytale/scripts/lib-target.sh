#!/usr/bin/env bash
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"
hytale_repo_root() { takaro_repo_root; }
hytale_resolve_target() { takaro_resolve_target hytale HYTALE "${1:-}"; }
hytale_parse_target_flag() { takaro_parse_target_flag "$@"; }
