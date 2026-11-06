#!/usr/bin/env bash
# Builds and runs the game-free tests with ASan and UBSan. Needs a C++20 compiler on Linux.
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"; trap 'rm -rf "$out"' EXIT
g++ -std=c++20 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
  client_test.cpp ../native/src/takaro_client.cpp ../native/src/transport.cpp -o "$out/client_test"
"$out/client_test"
g++ -std=c++20 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
  logic_test.cpp ../native/src/logic.cpp -o "$out/logic_test"
"$out/logic_test"
