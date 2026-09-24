#!/usr/bin/env bash
. "$(dirname -- "${BASH_SOURCE[0]}")/../../../scripts/lib/target.sh"
ark_repo_root() { takaro_repo_root; }
ark_resolve_target() { takaro_resolve_target ark ARK "${1:-}"; }
ark_parse_target_flag() { takaro_parse_target_flag "$@"; }
