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
$CC -O2 -fPIC -shared -Wl,-soname,libsteam.so shim/panel_loader.c -o "$T/b/libsteam.so" -ldl
$CC -O2 -fPIC -shared -DFAKE_TAG='"A"' tests/fake_connector.c -o "$T/a/libtakaro-vein.so"
$CC -O2 -fPIC -shared -DFAKE_TAG='"B"' tests/fake_connector.c -o "$T/b/libtakaro-vein.so"
fail() { echo "FAIL: $*"; echo "$out"; exit 1; }

out=$("$T/VeinServer-loader-test" "$PWD/$T/b/libsteam.so")
grep -q "FAKE CONNECTOR B LOADED" <<<"$out" || fail "loader did not load the connector next to it"
echo "panel loader loads the connector next to it: pass"

out=$(LD_PRELOAD="$PWD/$T/a/libtakaro-vein.so" "$T/VeinServer-loader-test" "$PWD/$T/b/libsteam.so")
grep -q "FAKE CONNECTOR A LOADED" <<<"$out" || fail "preloaded connector missing"
grep -q "FAKE CONNECTOR B" <<<"$out" && fail "loader loaded a second connector"
grep -q "already loaded" <<<"$out" || fail "loader gave no reason for skipping"
[ "$(grep -c '^MAPPED' <<<"$out")" = 1 ] || fail "expected exactly one mapped connector"
echo "panel loader skips when the connector is preloaded: pass"

out=$(cp "$T/VeinServer-loader-test" "$T/helper-process" && "$T/helper-process" "$PWD/$T/b/libsteam.so")
grep -q "FAKE CONNECTOR" <<<"$out" && fail "loader acted outside VeinServer"
echo "panel loader ignores other processes: pass"

# The real connector: the guard decides in the library constructor, so the host exits right after
# loading.
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

  # The real connector preloaded under another file name: the loader recognises it by its exported
  # marker, not by name, and loads nothing.
  mkdir -p "$T/renamed" "$T/real-b"
  cp dist/libtakaro-vein.so "$T/renamed/takaro-connector.so"
  cp dist/libtakaro-vein.so "$T/real-b/libtakaro-vein.so"; cp "$T/b/libsteam.so" "$T/real-b/libsteam.so"
  out=$(TAKARO_PLUGIN_DATA_DIR="$PWD/$T/data" TAKARO_NATIVE_DISABLE=1 \
        LD_PRELOAD="$PWD/$T/renamed/takaro-connector.so" \
        timeout 60 "$T/VeinServer-loader-test" "$PWD/$T/real-b/libsteam.so" 0)
  grep -q "panel loader: the connector is already loaded" <<<"$out" || fail "loader did not recognise a renamed connector"
  grep -q "real-b/libtakaro-vein.so" <<<"$out" && fail "loader loaded a second connector next to a renamed one"
  echo "panel loader recognises a preloaded connector under any file name: pass"
else
  echo "dist/libtakaro-vein.so missing: build first"; exit 1
fi

# Exit handlers flush the connector log after static destructors have run; the log path must still
# be intact then (a destroyed path once dropped files named after heap garbage into the game folder).
rm -rf "$T/cwd" "$T/data2"; mkdir -p "$T/cwd"
set +e
out=$(cd "$T/cwd" && TAKARO_PLUGIN_DATA_DIR="$PWD/../data2" TAKARO_NATIVE_DISABLE=1 \
      LD_PRELOAD="$PWD/../real1/libtakaro-vein.so" timeout 60 ../VeinServer-loader-test "" 1500 exit 2>&1)
rc=$?
set -e
[ "$rc" = 0 ] || fail "host exit code $rc"
stray=$(find "$T/cwd" -type f | wc -l)
[ "$stray" = 0 ] || { find "$T/cwd" -type f | cat -v; fail "connector wrote $stray stray file(s) into the working directory at exit"; }
grep -q "hooks: restored\|instance\|starting" "$T/data2/plugin.log" || fail "plugin.log missing its exit-time lines"
echo "exit-time log flush stays in plugin.log: pass"
