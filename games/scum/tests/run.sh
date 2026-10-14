#!/usr/bin/env bash
# Runs the TakaroSCUM Lua unit tests (fake UE4SS + fake Takaro). Needs a Lua 5.4 interpreter:
# LUA=/path/to/lua5.4 tests/run.sh   (default: lua5.4, then lua)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
src="$(cd "$here/.." && pwd)"
LUA="${LUA:-$(command -v lua5.4 || command -v lua || true)}"
if [ -z "$LUA" ]; then echo "no Lua 5.4 interpreter (set LUA=...)"; exit 2; fi
"$LUA" -v
export LUA_PATH="$src/mod/TakaroSCUM/Scripts/?.lua;$here/?.lua;;"
cd "$src" || exit 2
fail=0
for f in "$here"/test_*.lua; do
  "$LUA" "$f" || fail=1
done
if [ "$fail" = 0 ]; then echo "ALL LUA TESTS PASSED"; else echo "LUA TESTS FAILED"; fi
exit "$fail"
