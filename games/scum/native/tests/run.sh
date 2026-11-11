#!/usr/bin/env bash
# Builds and runs the native host tests (no Windows, no UE4SS): CXX=g++ games/scum/native/tests/run.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
native="$(cd "$here/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -I"$native/src" "$here/transport_test.cpp" "$native/src/transport.cpp" \
  -o "$out/transport_test"
"$out/transport_test"
