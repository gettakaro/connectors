#!/usr/bin/env bash
# Offline check of the gamedata derivation (src/derive.cpp) against real server binaries. Cross-compiles
# tests/derive_test.cpp with zig and runs it on Windows (from WSL), once per exe:
#   tests/derive_test.sh <enshrouded_server.exe> [more exes...]
# Each run prints the derived section; for a build that has a section in gamedata/enshrouded-gamedata.json the
# derived values are compared against it and every difference is listed.
set -euo pipefail
cd "$(dirname "$0")/.."
ZIG=${ZIG:-$HOME/.local/opt/zig-linux-x86_64-0.13.0/zig}
OUT=build/dt
RUN=${DERIVE_TEST_DIR:-/mnt/c/temp/derivetest}
mkdir -p "$OUT" "$RUN"
MH=third_party/minhook
# src/profile.cpp embeds the gamedata file, as build.sh does
{ printf 'R"GD('; cat gamedata/enshrouded-gamedata.json; printf ')GD"\n'; } > "$OUT/gamedata_default.inc"
"$ZIG" cc -target x86_64-windows-gnu -O2 -I$MH/include -c $MH/src/hde/hde64.c -o "$OUT/hde64.o"
"$ZIG" c++ -target x86_64-windows-gnu -O2 -std=c++17 -I$MH/include -Isrc -I"$OUT" -o "$OUT/derive_test.exe" \
  tests/derive_test.cpp src/derive.cpp src/scan.cpp src/profile.cpp src/common.cpp "$OUT/hde64.o"
cp "$OUT/derive_test.exe" gamedata/enshrouded-gamedata.json "$RUN/"
fail=0
for exe in "$@"; do
  cp "$exe" "$RUN/target.exe"
  out=$(cd "$RUN" && ./derive_test.exe target.exe enshrouded-gamedata.json) || { echo "FAIL $exe: derive failed"; fail=1; continue; }
  echo "$out" > "$RUN/derived.json"
  python3 -I - "$RUN/derived.json" gamedata/enshrouded-gamedata.json "$exe" <<'PY' || fail=1
import json, re, sys
d = json.load(open(sys.argv[1])); g = json.load(open(sys.argv[2]))["builds"]
n = lambda x: int(str(x), 0) if re.fullmatch(r"(0x)?[0-9a-fA-F]+", str(x)) else x
total = sum(len(d[s]) for s in ("functions", "callsites", "layout"))
for build, known in g.items():
    if known["functions"]["logSink"]["rva"] != d["functions"].get("logSink", {}).get("rva"): continue
    bad = [(s, k) for s in ("functions", "callsites", "layout") for k, v in known[s].items()
           if n(v["rva"] if s == "functions" else v) != n((d[s].get(k) or {}).get("rva") if s == "functions" else d[s].get(k))]
    for s, k in bad: print("DIFF", s, k)
    print(f"{sys.argv[3]}: build {build}, {total} values, {len(bad)} differ from the gamedata section")
    sys.exit(1 if bad else 0)
print(f"{sys.argv[3]}: derived {total} values (no gamedata section to compare with)")
PY
done
exit $fail
