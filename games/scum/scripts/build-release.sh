#!/usr/bin/env bash
# Builds the SCUM release zip for one catalog target:
#   takaro-scum-<target>-<version>.zip, unpacked by the operator into SCUM/Binaries/Win64.
#
#   games/scum/scripts/build-release.sh <version> <out-dir> [--target <id>]
#
# main.dll (the UE4SS C++ mod that holds the Takaro WebSocket) is cross-compiled with clang-cl and
# lld-link inside the pinned Debian image, against the MSVC CRT and Windows SDK that the pinned xwin
# fetches; that tree is checked against the catalog's digest before it is used. Every UE4SS import of
# main.dll is checked against the pinned UE4SS.dll, and the Lua tests run before anything is packaged.
# The zip carries the pinned UE4SS build (MIT, its LICENSE included) with the required hook settings,
# mods.txt with only TakaroSCUM enabled, and ue4ss/Mods/TakaroSCUM.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
repo_root=$(cd -- "$project_root/../.." && pwd)
. "$script_dir/lib-target.sh"

version="${1:?usage: build-release.sh <version> <out-dir> [--target id]}"
out_dir="${2:?usage: build-release.sh <version> <out-dir> [--target id]}"
shift 2
scum_parse_target_flag "$@"
scum_resolve_target "$TARGET"
case "$version" in *[!A-Za-z0-9._+-]*|"") echo "invalid version '$version'" >&2; exit 2;; esac
mkdir -p "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd)
epoch="${SOURCE_DATE_EPOCH:-$(git -C "$repo_root" log -1 --format=%ct)}"
revision="${TAKARO_SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}"

# The compiler packages, at exactly these versions from the pinned image's own release.
LLVM_VERSION="1:19.1.7-3+b1"
LUA_VERSION="5.4.7-1+b2"
CA_VERSION="20250419"
# MSVC CRT and Windows SDK versions, read from the catalog coordinate
# (microsoft:msvc-crt-<crt>+winsdk:<sdk>).
crt_version="${SCUM_DEP_MSVC_SYSROOT_COORDINATE#microsoft:msvc-crt-}"
crt_version="${crt_version%%+winsdk:*}"
sdk_version="${SCUM_DEP_MSVC_SYSROOT_COORDINATE##*+winsdk:}"

cache="${TAKARO_MAINT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/takaro-maint}/scum"
mkdir -p "$cache/downloads"

fetch() { # fetch <url> <sha256> <file>
    local url="$1" sha="$2" file="$cache/downloads/$3"
    if [ ! -f "$file" ] || ! echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
        curl -fsSL --retry 3 "$url" -o "$file.part"
        mv "$file.part" "$file"
    fi
    echo "$sha  $file" | sha256sum -c --quiet -
}
fetch "$SCUM_DEP_XWIN_URL" "$SCUM_DEP_XWIN_SHA256" xwin.tar.gz
fetch "$SCUM_DEP_UE4SS_URL" "$SCUM_DEP_UE4SS_SHA256" ue4ss.zip

stage="$project_root/_data/build/stage-${SCUM_FP16}"
rm -rf "$stage"
mkdir -p "$stage/native"
cp -r "$project_root/native/src" "$project_root/native/ue4ss" "$stage/native/"

# Compile inside the pinned image (as root for apt; the outputs are handed back to the caller).
docker run --rm -e HOME=/tmp -e SOURCE_DATE_EPOCH="$epoch" \
  -e LLVM_VERSION="$LLVM_VERSION" -e LUA_VERSION="$LUA_VERSION" -e CA_VERSION="$CA_VERSION" \
  -e CRT_VERSION="$crt_version" -e SDK_VERSION="$sdk_version" \
  -e SYSROOT_SHA256="$SCUM_DEP_MSVC_SYSROOT_SHA256" -e VERSION="$version" \
  -e OWNER="$(id -u):$(id -g)" \
  -v "$cache:/cache" -v "$project_root:/project:ro" -v "$stage:/stage" \
  "$SCUM_TOOLCHAIN" bash -c '
    set -euo pipefail
    trap "chown -R $OWNER /stage /cache" EXIT
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    apt-get install -y -qq --no-install-recommends \
      "clang-19=$LLVM_VERSION" "lld-19=$LLVM_VERSION" "llvm-19=$LLVM_VERSION" "lua5.4=$LUA_VERSION" "ca-certificates=$CA_VERSION" >/dev/null

    echo "== Lua tests"
    cp -r /project/mod /project/tests /project/capabilities.json /project/TakaroConfig.txt.example /stage/
    (cd /stage && LUA=lua5.4 bash tests/run.sh)

    sysroot="/cache/msvc-$CRT_VERSION-$SDK_VERSION"
    digest() { (cd "$1" && find crt sdk -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d" " -f1); }
    if [ ! -d "$sysroot" ] || [ "$(digest "$sysroot")" != "$SYSROOT_SHA256" ]; then
      echo "== fetching MSVC CRT $CRT_VERSION and Windows SDK $SDK_VERSION (xwin)"
      rm -rf "$sysroot" /tmp/xwin /cache/xwin-cache && mkdir -p /tmp/xwin
      tar -xzf /cache/downloads/xwin.tar.gz -C /tmp/xwin --strip-components=1
      /tmp/xwin/xwin --accept-license --arch x86_64 --crt-version "$CRT_VERSION" --sdk-version "$SDK_VERSION" \
        --http-retry 3 --cache-dir /cache/xwin-cache splat --output "$sysroot" >/dev/null
      rm -rf /cache/xwin-cache
    fi
    got="$(digest "$sysroot")"
    if [ "$got" != "$SYSROOT_SHA256" ]; then
      echo "MSVC sysroot digest $got != catalog $SYSROOT_SHA256" >&2; exit 1
    fi

    echo "== main.dll (clang-cl $LLVM_VERSION)"
    cd /stage/native && mkdir -p build
    inc=(-imsvc "$sysroot/crt/include" -imsvc "$sysroot/sdk/include/ucrt" -imsvc "$sysroot/sdk/include/um"
         -imsvc "$sysroot/sdk/include/shared")
    libs=("/libpath:$sysroot/crt/lib/x86_64" "/libpath:$sysroot/sdk/lib/um/x86_64" "/libpath:$sysroot/sdk/lib/ucrt/x86_64")
    cflags=(--target=x86_64-pc-windows-msvc -fuse-ld=lld /std:c++20 /O2 /MD /EHsc /W3 /wd4996 /DNDEBUG /DUNICODE
            /D_UNICODE /utf-8 /Brepro "-DTAKARO_PLUGIN_VERSION=\"$VERSION\"")
    llvm-dlltool-19 -m i386:x86-64 -d ue4ss/UE4SS.def -l build/UE4SS.lib
    clang-19 --driver-mode=cl "${cflags[@]}" "${inc[@]}" /LD /Fobuild/ /Febuild/main.dll \
      src/mod_main.cpp src/transport.cpp src/transport_winhttp.cpp src/bridge.cpp src/log.cpp \
      /link "${libs[@]}" /Brepro /OPT:REF /OPT:ICF build/UE4SS.lib winhttp.lib crypt32.lib
  '

dll="$stage/native/build/main.dll"
echo "== UE4SS import check"
ue4ss_dir="$stage/ue4ss-pinned"
mkdir -p "$ue4ss_dir"
python3 -I -c 'import sys, zipfile; open(sys.argv[2], "wb").write(zipfile.ZipFile(sys.argv[1]).read("ue4ss/UE4SS.dll"))' \
  "$cache/downloads/ue4ss.zip" "$ue4ss_dir/UE4SS.dll"
python3 -I "$project_root/native/check-imports.py" "$dll" "$ue4ss_dir/UE4SS.dll"

echo "== package"
name="${SCUM_ARTIFACT_MOD/\{version\}/$version}"
SCUM_VERSION="$version" SCUM_EPOCH="$epoch" SCUM_REVISION_SHA="$revision" \
python3 -I - "$project_root" "$dll" "$out_dir/$name" "$cache/downloads/ue4ss.zip" <<'PY'
import hashlib, json, os, re, sys, time, zipfile
project, dll, dest, ue_zip = sys.argv[1:5]
version = os.environ["SCUM_VERSION"]
coordinate = os.environ["SCUM_DEP_UE4SS_COORDINATE"]
ue_name = "UE4SS_" + coordinate.rsplit(":", 1)[1] + ".zip"
stamp = time.gmtime(max(int(os.environ["SCUM_EPOCH"]), 315532800))[:6]
MOD = "ue4ss/Mods/TakaroSCUM"

required = {}
for line in open(os.path.join(project, "native", "ue4ss", "UE4SS-settings.required.txt"), encoding="utf-8"):
    m = re.match(r"^\s*([A-Za-z0-9_]+)\s*=\s*(\S+)", line)
    if m:
        required[m.group(1)] = m.group(2)

def patch_settings(text):
    seen = set()
    def sub(m):
        if m.group(2) in required:
            seen.add(m.group(2))
            return f"{m.group(1)}{m.group(2)} = {required[m.group(2)]}"
        return m.group(0)
    text = re.sub(r"(?m)^(\s*)([A-Za-z0-9_]+)\s*=\s*\S*", sub, text)
    missing = set(required) - seen
    if missing:
        sys.exit(f"UE4SS-settings.ini has no keys {sorted(missing)}")
    return text

files = {}
with zipfile.ZipFile(ue_zip) as ue:
    stock_mods = []
    for name in ue.namelist():
        if name.endswith("/"):
            continue
        data = ue.read(name)
        if name == "ue4ss/UE4SS-settings.ini":
            data = patch_settings(data.decode("utf-8-sig")).encode("utf-8")
        elif name == "ue4ss/Mods/mods.txt":
            for line in data.decode("utf-8-sig").splitlines():
                m = re.match(r"^\s*([^;:\s][^:]*?)\s*:\s*\d", line)
                if m:
                    stock_mods.append(m.group(1))
            continue
        elif name == "ue4ss/Mods/mods.json":
            continue
        files[name] = data
files["ue4ss/Mods/mods.txt"] = ("".join(f"{m} : 0\r\n" for m in stock_mods if m != "TakaroSCUM")
                                + "TakaroSCUM : 1\r\n").encode("utf-8")

scripts = os.path.join(project, "mod", "TakaroSCUM")
for root, _dirs, names in os.walk(scripts):
    for f in names:
        path = os.path.join(root, f)
        rel = os.path.relpath(path, scripts).replace(os.sep, "/")
        data = open(path, "rb").read()
        if rel == "Scripts/takaro/app.lua":
            data, n = re.subn(rb'M\.VERSION = "[^"]*"', b'M.VERSION = "' + version.encode() + b'"', data)
            if n != 1:
                sys.exit("app.lua: expected exactly one M.VERSION line")
        files[f"{MOD}/{rel}"] = data
files[f"{MOD}/dlls/main.dll"] = open(dll, "rb").read()
files[f"{MOD}/TakaroConfig.txt.example"] = open(os.path.join(project, "TakaroConfig.txt.example"), "rb").read()
files[f"{MOD}/VERSION"] = (version + "\n").encode()
identity = {
    "target": os.environ["SCUM_TARGET"], "fingerprint": os.environ["SCUM_FINGERPRINT"],
    "connectorVersion": version, "sourceRevision": os.environ["SCUM_REVISION_SHA"],
    "game": "scum", "platform": os.environ["SCUM_PLATFORM"], "revision": os.environ["SCUM_REVISION"],
}
files[f"{MOD}/takaro-target.json"] = (json.dumps(identity, sort_keys=True) + "\n").encode()
dll_sha = hashlib.sha256(files[f"{MOD}/dlls/main.dll"]).hexdigest()
files["THIRD-PARTY-NOTICES.txt"] = (
    f"dwmapi.dll and ue4ss/ (except {MOD}) are UE4SS {ue_name}\r\n"
    "from https://github.com/UE4SS-RE/RE-UE4SS, MIT licence: see ue4ss/LICENSE.\r\n"
    "Unmodified except ue4ss/UE4SS-settings.ini (hook settings, see PROVENANCE.txt)\r\n"
    "and ue4ss/Mods/mods.txt (only TakaroSCUM enabled).\r\n"
    f"{MOD}/Scripts/takaro/json.lua is rxi/json.lua (MIT), with small changes.\r\n").encode()
files["PROVENANCE.txt"] = (
    f"takaro-scum {version} for SCUM Steam build {os.environ['SCUM_REVISION']}\r\n"
    f"UE4SS: {ue_name} sha256 {os.environ['SCUM_DEP_UE4SS_SHA256']}\r\n"
    f"main.dll sha256 {dll_sha}\r\n"
    "UE4SS-settings.ini overrides (the stock hook set crashes SCUMServer on player join):\r\n"
    + "".join(f"  {k} = {v}\r\n" for k, v in required.items())).encode()

for name in files:
    if name.rsplit("/", 1)[-1] in ("TakaroConfig.txt", "TakaroIdentity.txt", "known-players.json", "takaro-bans.json"):
        sys.exit(f"refusing to package operator state {name}")
with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in sorted(files):
        entry = zipfile.ZipInfo(name, stamp)
        entry.compress_type = zipfile.ZIP_DEFLATED
        entry.external_attr = (0o100644 << 16)
        z.writestr(entry, files[name], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
meta = {k: identity[k] for k in ("target", "fingerprint", "connectorVersion", "sourceRevision", "game", "platform",
                                 "revision")}
open(dest + ".meta.json", "w").write(json.dumps(meta, sort_keys=True) + "\n")
PY
sha256sum "$dll" "$out_dir/$name"
