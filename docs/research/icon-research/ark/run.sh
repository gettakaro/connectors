#!/usr/bin/env bash
# Spike #813 tooling: build the pinned images and run every decode or parse step network-less.
#
#   run.sh build
#   run.sh umodel --in DIR --out DIR -- UMODEL_ARGS...      (DIR/in is mounted at /in:ro, /out is writable)
#   run.sh reader --in DIR --out DIR -- ARK_UASSET_ARGS...      (info, codes, map; paths under /in and /out)
#   run.sh sheet  --in DIR --out DIR                         (DDS -> PNG, statistics, contact sheet; --in is the reader's --out)
#
# Game data lives in a cache outside the repository. --in and --out inside the git worktree are refused,
# so nothing the tools write can end up in a commit.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UMODEL_IMAGE="ark-spike-umodel:0"
PY_IMAGE="ark-spike-py:0"
PLAIN_PY_IMAGE="python:3.12-slim-bookworm@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df"

usage() {
  sed -n '2,9p' "${BASH_SOURCE[0]}" >&2
  exit 2
}

refuse_inside_worktree() {
  local top path
  top="$(git -C "$HERE" rev-parse --show-toplevel)"
  path="$(realpath -m "$1")"
  case "$path/" in
    "$top"/*) echo "refusing $1: inside the git worktree $top" >&2; exit 1 ;;
  esac
}

sandbox=(
  docker run --rm
  --network none
  --read-only
  --tmpfs /tmp
  --cap-drop ALL
  --security-opt no-new-privileges
  --user "$(id -u):$(id -g)"
)

command_name="${1:-}"
[ -n "$command_name" ] || usage
shift

if [ "$command_name" = build ]; then
  docker build -f "$HERE/Dockerfile.umodel" -t "$UMODEL_IMAGE" "$HERE"
  docker build -f "$HERE/Dockerfile.py" -t "$PY_IMAGE" "$HERE"
  exit 0
fi

IN="" OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --in) IN="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --) shift; break ;;
    *) break ;;
  esac
done
if [ -z "$IN" ] || [ -z "$OUT" ]; then usage; fi
refuse_inside_worktree "$IN"
refuse_inside_worktree "$OUT"
IN="$(realpath "$IN")"
mkdir -p "$OUT"
OUT="$(realpath "$OUT")"

case "$command_name" in
  umodel)
    "${sandbox[@]}" -v "$IN":/in:ro -v "$OUT":/out "$UMODEL_IMAGE" umodel "$@"
    ;;
  reader)
    # stdlib only, so the plain digest-pinned python image is enough; -I ignores the environment
    # and the current directory.
    "${sandbox[@]}" -v "$IN":/in:ro -v "$OUT":/out -v "$HERE":/spike:ro "$PLAIN_PY_IMAGE" \
      python -I /spike/ark_uasset.py "$@"
    ;;
  sheet)
    "${sandbox[@]}" -v "$IN":/in:ro -v "$OUT":/out -v "$HERE":/spike:ro "$PY_IMAGE" \
      python -I /spike/sheet.py --dds-dir /in/dds --out /out/sheet.png --stats /out/png-stats.json
    ;;
  *) usage ;;
esac
