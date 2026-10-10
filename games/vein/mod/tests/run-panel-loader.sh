#!/usr/bin/env bash
# Panel loader + single-instance guard: the loader loads the connector next to it, skips when a
# connector is already mapped (LD_PRELOAD), and a second real connector copy refuses to start.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--native" ]; then
  docker build -q -t takaro-vein-build -f Dockerfile.build . >/dev/null
  exec docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" takaro-vein-build ./tests/run-panel-loader.sh --native
fi
CC=${CC:-gcc}
T=tests/build/panel-loader
rm -rf "$T"; mkdir -p "$T/a" "$T/b" "$T/real1" "$T/real2"
$CC -O2 -Wall -Wextra tests/panel_loader_host.c -o "$T/VeinServer-loader-test" -ldl
$CC -O2 -fPIC -shared -Wl,-soname,libSDL3.so.0 shim/panel_loader.c -o "$T/b/libSDL3.so.0" -ldl
$CC -O2 -fPIC -shared -DFAKE_TAG='"A"' tests/fake_connector.c -o "$T/a/libtakaro-vein.so"
$CC -O2 -fPIC -shared -DFAKE_TAG='"B"' tests/fake_connector.c -o "$T/b/libtakaro-vein.so"
fail() { echo "FAIL: $*"; echo "$out"; exit 1; }

out=$("$T/VeinServer-loader-test" "$PWD/$T/b/libSDL3.so.0")
grep -q "FAKE CONNECTOR B LOADED" <<<"$out" || fail "loader did not load the connector next to it"
echo "panel loader loads the connector next to it: pass"

out=$(LD_PRELOAD="$PWD/$T/a/libtakaro-vein.so" "$T/VeinServer-loader-test" "$PWD/$T/b/libSDL3.so.0")
grep -q "FAKE CONNECTOR A LOADED" <<<"$out" || fail "preloaded connector missing"
grep -q "FAKE CONNECTOR B" <<<"$out" && fail "loader loaded a second connector"
grep -q "already loaded" <<<"$out" || fail "loader gave no reason for skipping"
[ "$(grep -c '^MAPPED' <<<"$out")" = 1 ] || fail "expected exactly one mapped connector"
echo "panel loader skips when the connector is preloaded: pass"

out=$(cp "$T/VeinServer-loader-test" "$T/helper-process" && "$T/helper-process" "$PWD/$T/b/libSDL3.so.0")
grep -q "FAKE CONNECTOR" <<<"$out" && fail "loader acted outside VeinServer"
echo "panel loader ignores other processes: pass"

# The real connector: the guard decides in the library constructor, so the host exits right after
# loading (the start-up thread is not meant to run in this fake game; it has crashed it now and then).
# Two copies preloaded from different folders: exactly one stays idle.
if [ -f dist/libtakaro-vein.so ]; then
  cp dist/libtakaro-vein.so "$T/real1/"; cp dist/libtakaro-vein.so "$T/real2/"
  out=$(TAKARO_PLUGIN_DATA_DIR="$PWD/$T/data" TAKARO_NATIVE_DISABLE=1 \
        LD_PRELOAD="$PWD/$T/real1/libtakaro-vein.so $PWD/$T/real2/libtakaro-vein.so" \
        timeout 60 "$T/VeinServer-loader-test" "" 0)
  [ "$(grep -c 'connector not started twice' <<<"$out")" = 1 ] || fail "exactly one real copy must refuse"
  echo "two preloaded real copies: exactly one stays idle: pass"

  # An older release (no guard symbol) is preloaded: the new copy stays idle instead of doubling.
  out=$(TAKARO_PLUGIN_DATA_DIR="$PWD/$T/data" TAKARO_NATIVE_DISABLE=1 \
        LD_PRELOAD="$PWD/$T/a/libtakaro-vein.so $PWD/$T/real1/libtakaro-vein.so" \
        timeout 60 "$T/VeinServer-loader-test" "" 0)
  grep -q "FAKE CONNECTOR A LOADED" <<<"$out" || fail "old copy missing"
  grep -q "an older connector (.*/a/libtakaro-vein.so) is already loaded" <<<"$out" || fail "new copy ran next to an old one"
  grep -q "real1/libtakaro-vein.so stays idle" <<<"$out" || fail "new copy did not stay idle"
  echo "a new copy stays idle next to an older release: pass"
else
  echo "dist/libtakaro-vein.so missing: build first"; exit 1
fi
