#!/usr/bin/env bash
# Offline check of the item-table rebuild (src/items_kfc.cpp) against a real server install. Cross-compiles
# tests/items_kfc_test.cpp with zig and runs it on Windows (from WSL):
#   tests/items_kfc_test.sh <folder with enshrouded_server.exe, .kfc and .kfc_resources>
# Prints the ItemInfo layout read from the exe and how the rebuilt table compares with src/gamedata.cpp, then runs the
# startup path twice in a scratch folder: the first start rebuilds and saves takaro/enshrouded-items.json, the second
# reads it.
set -euo pipefail
cd "$(dirname "$0")/.."
ZIG=${ZIG:-$HOME/.local/opt/zig-linux-x86_64-0.13.0/zig}
OUT=build/it
RUN=${ITEMS_TEST_DIR:-/mnt/c/temp/itemstest}
mkdir -p "$OUT" "$RUN"
"$ZIG" cc -target x86_64-windows-gnu -O2 -c third_party/zstd/zstddeclib.c -o "$OUT/zstddeclib.o"
"$ZIG" c++ -target x86_64-windows-gnu -O2 -std=c++17 -Isrc -o "$OUT/items_kfc_test.exe" \
  tests/items_kfc_test.cpp src/items_kfc.cpp src/scan.cpp src/common.cpp src/gamedata.cpp "$OUT/zstddeclib.o"
cp "$OUT/items_kfc_test.exe" "$RUN/"
cp "$1/enshrouded_server.exe" "$RUN/target.exe"
cd "$RUN"
./items_kfc_test.exe target.exe "$(wslpath -w "$1/enshrouded_server.kfc")"
cp "$1/enshrouded_server.kfc" "$1/enshrouded_server.kfc_resources" .
rm -rf takaro && mkdir takaro
./items_kfc_test.exe target.exe --load 9999999
./items_kfc_test.exe target.exe --load 9999999
python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print("saved:", d["gameBuild"], len(d["items"]), "rows;", d["note"])' takaro/enshrouded-items.json
