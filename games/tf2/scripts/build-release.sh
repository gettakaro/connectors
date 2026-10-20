#!/usr/bin/env bash
# Builds the Team Fortress 2 release zip for one catalog target:
#   takaro-tf2-<target>-<version>.zip, unpacked by the operator into the server's tf/ folder.
#
#   games/tf2/scripts/build-release.sh <version> <out-dir> [--target <id>]
#
# The plugin is compiled with the pinned SourceMod 1.13 spcomp64 inside the pinned Debian image,
# against the Linux ProjectSky extension includes, with zero warnings allowed. A .smx runs on both
# Windows and Linux, so both platform zips carry the same plugin and differ only in the extension
# binaries (32- and 64-bit). Every download is pinned by SHA-256 in the catalog target and cached
# under $TAKARO_MAINT_CACHE/tf2.
#
# The zip holds addons/sourcemod/{plugins,extensions,scripting,licenses}, the config template
# cfg/sourcemod/takaro_tf2.cfg and addons/sourcemod/takaro-target.json (the target identity).
# Set SPCOMP to compile with a local SourceMod compiler instead of the pinned image (development only).
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
repo_root=$(cd -- "$project_root/../.." && pwd)
. "$script_dir/lib-target.sh"

usage="usage: build-release.sh <version> <out-dir> [--target id]"
version="${1:?$usage}"
out_dir="${2:?$usage}"
shift 2
tf2_parse_target_flag "$@"
tf2_resolve_target "$TARGET"
case "$version" in *[!A-Za-z0-9._+-]*|"") echo "invalid version '$version'" >&2; exit 2;; esac
case "$TF2_PLATFORM" in linux|windows) ;; *) echo "unknown platform '$TF2_PLATFORM'" >&2; exit 2;; esac
mkdir -p "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd)
epoch="${SOURCE_DATE_EPOCH:-$(git -C "$repo_root" log -1 --format=%ct)}"
revision="${TAKARO_SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}"

cache="${TAKARO_MAINT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/takaro-maint}/tf2"
mkdir -p "$cache/downloads"

fetch() { # fetch <url> <sha256> <file>
    local url="$1" sha="$2" file="$cache/downloads/$3"
    if [ ! -f "$file" ] || ! echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
        curl -fsSL --retry 3 "$url" -o "$file.part"
        mv "$file.part" "$file"
    fi
    echo "$sha  $file" | sha256sum -c --quiet - || { echo "checksum mismatch: $url" >&2; exit 1; }
}
fetch "$TF2_DEP_SOURCEMOD_URL" "$TF2_DEP_SOURCEMOD_SHA256" sourcemod-linux.tar.gz
fetch "$TF2_DEP_WEBSOCKET_LINUX_URL" "$TF2_DEP_WEBSOCKET_LINUX_SHA256" websocket-linux.zip
fetch "$TF2_DEP_JSON_LINUX_URL" "$TF2_DEP_JSON_LINUX_SHA256" json-linux.zip
if [ "$TF2_PLATFORM" = windows ]; then
    fetch "$TF2_DEP_WEBSOCKET_WINDOWS_URL" "$TF2_DEP_WEBSOCKET_WINDOWS_SHA256" websocket-windows.zip
    fetch "$TF2_DEP_JSON_WINDOWS_URL" "$TF2_DEP_JSON_WINDOWS_SHA256" json-windows.zip
fi
fetch "$TF2_DEP_WEBSOCKET_LICENSE_URL" "$TF2_DEP_WEBSOCKET_LICENSE_SHA256" LICENSE-sm-ext-websocket.txt
fetch "$TF2_DEP_JSON_LICENSE_URL" "$TF2_DEP_JSON_LICENSE_SHA256" LICENSE-sm-ext-json.txt

stage="$project_root/_data/build/stage-${TF2_FP16}"
rm -rf "$stage"
mkdir -p "$stage/src" "$stage/include" "$stage/sm" "$stage/out"
unzip_to() { python3 -I -m zipfile -e "$1" "$2"; }
for ext in websocket json; do
    mkdir -p "$stage/ext-$ext-linux"
    unzip_to "$cache/downloads/$ext-linux.zip" "$stage/ext-$ext-linux"
    cp -r "$stage/ext-$ext-linux/addons/sourcemod/scripting/include/." "$stage/include/"
    if [ "$TF2_PLATFORM" = windows ]; then
        mkdir -p "$stage/ext-$ext-windows"
        unzip_to "$cache/downloads/$ext-windows.zip" "$stage/ext-$ext-windows"
    fi
done
tar -xzf "$cache/downloads/sourcemod-linux.tar.gz" -C "$stage/sm" addons/sourcemod/scripting

# The release version goes into the plugin the server reports; the tracked source keeps its own.
scripting="$project_root/mod/addons/sourcemod/scripting"
cp "$scripting/takaro_tf2.sp" "$scripting/takaro_tf2_item_catalog.inc" "$stage/src/"
python3 -I - "$stage/src/takaro_tf2.sp" "$version" <<'PY'
import re, sys
path, version = sys.argv[1:3]
text = open(path, encoding="utf-8").read()
text, n = re.subn(r'(?m)^#define PLUGIN_VERSION "[^"]*"', f'#define PLUGIN_VERSION "{version}"', text)
if n != 1:
    sys.exit("takaro_tf2.sp: expected exactly one PLUGIN_VERSION line")
open(path, "w", encoding="utf-8").write(text)
PY

echo "== takaro_tf2.smx (SourceMod spcomp64)"
if [ -n "${SPCOMP:-}" ]; then
    "$SPCOMP" "$stage/src/takaro_tf2.sp" -i "$stage/src" -i "$stage/include" \
        -i "$stage/sm/addons/sourcemod/scripting/include" -o "$stage/out/takaro_tf2.smx" >"$stage/spcomp.log" 2>&1 \
        || { cat "$stage/spcomp.log" >&2; exit 1; }
else
    docker run --rm -e OWNER="$(id -u):$(id -g)" -v "$stage:/stage" "$TF2_TOOLCHAIN" bash -c '
        set -euo pipefail
        trap "chown -R $OWNER /stage" EXIT
        cd /stage
        sm/addons/sourcemod/scripting/spcomp64 src/takaro_tf2.sp -i src -i include \
            -i sm/addons/sourcemod/scripting/include -o out/takaro_tf2.smx >spcomp.log 2>&1 \
            || { cat spcomp.log >&2; exit 1; }
    '
fi
if grep -qi "warning" "$stage/spcomp.log"; then
    cat "$stage/spcomp.log" >&2
    echo "spcomp warnings are not allowed" >&2
    exit 1
fi
# spcomp stamps the wall-clock __DATE__/__TIME__ into the plugin's version block (SourceMod's
# core.inc) and has no SOURCE_DATE_EPOCH support. The .smx is uncompressed and unchecksummed, so the
# two same-length strings are set to the commit time; the build is then byte-for-byte reproducible.
python3 -I - "$stage/out/takaro_tf2.smx" "$epoch" <<'PY'
import re, sys, time
path, epoch = sys.argv[1], int(sys.argv[2])
data = open(path, "rb").read()
if data[:4] != b"FFPS" or data[6] != 0:
    sys.exit("takaro_tf2.smx: not an uncompressed SMX file")
stamp = time.gmtime(epoch)
date, clock = time.strftime("%m/%d/%Y", stamp).encode(), time.strftime("%H:%M:%S", stamp).encode()
data, n = re.subn(rb"\n[0-9]{2}/[0-9]{2}/[0-9]{4}\x08[0-9]{2}:[0-9]{2}:[0-9]{2}", b"\n" + date + b"\x08" + clock, data)
if n != 1:
    sys.exit(f"takaro_tf2.smx: expected one compile date/time stamp, found {n}")
open(path, "wb").write(data)
PY

echo "== package"
name="${TF2_ARTIFACT_PLUGIN/\{version\}/$version}"
TF2_VERSION="$version" TF2_EPOCH="$epoch" TF2_REVISION_SHA="$revision" \
python3 -I - "$project_root" "$stage" "$cache/downloads" "$out_dir/$name" <<'PY'
import json, os, sys, time, zipfile
project, stage, downloads, dest = sys.argv[1:5]
platform = os.environ["TF2_PLATFORM"]
stamp = time.gmtime(max(int(os.environ["TF2_EPOCH"]), 315532800))[:6]
SM = "addons/sourcemod"

files = {}
for ext in ("websocket", "json"):
    root = os.path.join(stage, f"ext-{ext}-{platform}", SM, "extensions")
    for dirpath, _dirs, names in os.walk(root):
        for f in names:
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            files[f"{SM}/extensions/{rel}"] = open(path, "rb").read()
files[f"{SM}/plugins/takaro_tf2.smx"] = open(os.path.join(stage, "out", "takaro_tf2.smx"), "rb").read()
for f in ("takaro_tf2.sp", "takaro_tf2_item_catalog.inc"):
    files[f"{SM}/scripting/{f}"] = open(os.path.join(stage, "src", f), "rb").read()
files["cfg/sourcemod/takaro_tf2.cfg"] = open(os.path.join(project, "mod", "cfg", "sourcemod", "takaro_tf2.cfg"), "rb").read()
for f in ("LICENSE-sm-ext-websocket.txt", "LICENSE-sm-ext-json.txt"):
    files[f"{SM}/licenses/{f}"] = open(os.path.join(downloads, f), "rb").read()
identity = {
    "target": os.environ["TF2_TARGET"], "fingerprint": os.environ["TF2_FINGERPRINT"],
    "connectorVersion": os.environ["TF2_VERSION"], "sourceRevision": os.environ["TF2_REVISION_SHA"],
    "game": "tf2", "platform": platform, "revision": os.environ["TF2_REVISION"],
}
files[f"{SM}/takaro-target.json"] = (json.dumps(identity, sort_keys=True) + "\n").encode()

expected = ".ext.so" if platform == "linux" else ".ext.dll"
for ext in ("websocket", "json"):
    for arch in ("", "x64/"):
        if f"{SM}/extensions/{arch}{ext}{expected}" not in files:
            sys.exit(f"the {ext} extension zip has no {arch}{ext}{expected}")
if f"{SM}/data/takaro_tf2_bans.txt" in files:
    sys.exit("refusing to package connector state")

with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in sorted(files):
        entry = zipfile.ZipInfo(name, stamp)
        entry.compress_type = zipfile.ZIP_DEFLATED
        entry.external_attr = 0o100644 << 16
        z.writestr(entry, files[name], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
open(dest + ".meta.json", "w").write(json.dumps(identity, sort_keys=True) + "\n")
PY
sha256sum "$stage/out/takaro_tf2.smx" "$out_dir/$name"
