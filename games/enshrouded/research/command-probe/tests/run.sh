#!/usr/bin/env bash
# Host test for the #202 command probe's decision core (same pinned gcc image as mod/tests/run.sh).
set -euo pipefail
cd "$(dirname "$0")/../../.."
docker run --rm -v "$PWD":/p gcc:14@sha256:cb57ac6c7917425c057c736fe3a240df25bce310418d61cd223bc8e411364876 sh -c \
  "cd /p && g++ -std=c++17 -Wall -Imod/tests/shim -Imod/src -Iresearch/command-probe -include cstdarg \
   research/command-probe/tests/probe_core_test.cpp research/command-probe/probe_core.cpp mod/src/common.cpp \
   -o /tmp/probe && /tmp/probe"
