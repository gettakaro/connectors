#!/usr/bin/env bash
# Packages the ARK: Survival Ascended release zip for one catalog target:
#   takaro-ark-ascended-<target>-<version>.zip, unpacked by the operator into ShooterGame/Binaries/Win64.
#
#   games/ark-ascended/scripts/build-release.sh <version> <out-dir> [--target <id>]
#
# TakaroAsa.dll is compiled by MSVC on a Windows runner (see .github/workflows/ark-ascended.yml and
# native/build.bat), because AsaApi's headers and AsaApi.lib target MSVC. This script does not compile:
# it takes the DLL from _data/prebuilt/<target>/, or from TAKARO_ASA_DLL, refuses one built for another
# version, and bundles it with the pinned AsaApi release, whose zip is checked against the catalog.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
repo_root=$(cd -- "$project_root/../.." && pwd)
. "$script_dir/lib-target.sh"

version="${1:?usage: build-release.sh <version> <out-dir> [--target id]}"
out_dir="${2:?usage: build-release.sh <version> <out-dir> [--target id]}"
shift 2
asa_parse_target_flag "$@"
asa_resolve_target "$TARGET"
case "$version" in *[!A-Za-z0-9._+-]*|"") echo "invalid version '$version'" >&2; exit 2;; esac
mkdir -p "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd)
epoch="${SOURCE_DATE_EPOCH:-$(git -C "$repo_root" log -1 --format=%ct)}"
revision="${TAKARO_SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}"

dll="${TAKARO_ASA_DLL:-$project_root/_data/prebuilt/$ARK_ASCENDED_TARGET/TakaroAsa.dll}"
if [ ! -f "$dll" ]; then
    echo "no TakaroAsa.dll at $dll: build it with native/build.bat on Windows first" >&2
    exit 1
fi

cache="${TAKARO_MAINT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/takaro-maint}/ark-ascended"
mkdir -p "$cache/downloads"

fetch() { # fetch <url> <sha256> <file>
    local url="$1" sha="$2" file="$cache/downloads/$3"
    if [ ! -f "$file" ] || ! echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
        curl -fsSL --retry 3 "$url" -o "$file.part"
        mv "$file.part" "$file"
    fi
    echo "$sha  $file" | sha256sum -c --quiet -
}
fetch "$ARK_ASCENDED_DEP_ASAAPI_URL" "$ARK_ASCENDED_DEP_ASAAPI_SHA256" asaapi.zip
fetch "$ARK_ASCENDED_DEP_ASAAPI_SOURCE_URL" "$ARK_ASCENDED_DEP_ASAAPI_SOURCE_SHA256" asaapi-source.tar.gz

echo "== package"
name="${ARK_ASCENDED_ARTIFACT_MOD/\{version\}/$version}"
ASA_VERSION="$version" ASA_EPOCH="$epoch" ASA_REVISION_SHA="$revision" \
python3 -I - "$project_root" "$dll" "$out_dir/$name" "$cache/downloads/asaapi.zip" "$cache/downloads/asaapi-source.tar.gz" <<'PY'
import hashlib, json, os, struct, sys, tarfile, time, zipfile

project, dll_path, dest, asa_zip, asa_source = sys.argv[1:6]
version = os.environ["ASA_VERSION"]
stamp = time.gmtime(max(int(os.environ["ASA_EPOCH"]), 315532800))[:6]
PLUGIN = "ArkApi/Plugins/TakaroAsa"
# Development files of the AsaApi release that a server never reads.
DEVELOPER_ONLY = ("Lib/", "ArkApi/Plugins/Permissions/ONLY FOR DEVELOPERS/")

dll = open(dll_path, "rb").read()
pe = struct.unpack_from("<I", dll, 0x3C)[0] if dll[:2] == b"MZ" else None
if pe is None or dll[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", dll, pe + 4)[0] != 0x8664:
    sys.exit("TakaroAsa.dll is not an x64 PE file")
if b"Plugin_Init\0" not in dll:
    sys.exit("TakaroAsa.dll does not export Plugin_Init")
if f"TakaroAsa {version} starting".encode() not in dll:
    sys.exit(f"TakaroAsa.dll was not built for version {version}")

files = {}
with zipfile.ZipFile(asa_zip) as upstream:
    for entry in upstream.namelist():
        if not entry.endswith("/") and not entry.startswith(DEVELOPER_ONLY):
            files[entry] = upstream.read(entry)
with tarfile.open(asa_source) as source:
    licence = next(m for m in source.getmembers() if m.name.count("/") == 1 and m.name.endswith("/LICENSE"))
    files["ArkApi/LICENSE-AsaApi.txt"] = source.extractfile(licence).read()

files[f"{PLUGIN}/TakaroAsa.dll"] = dll
for packaged, source_file in (("PluginInfo.json", "native/PluginInfo.json"), ("icon-256.png", "assets/icon-256.png"),
                              ("TakaroConfig.txt.example", "TakaroConfig.txt.example")):
    files[f"{PLUGIN}/{packaged}"] = open(os.path.join(project, source_file), "rb").read()
files[f"{PLUGIN}/VERSION"] = (version + "\n").encode()
identity = {
    "target": os.environ["ARK_ASCENDED_TARGET"], "fingerprint": os.environ["ARK_ASCENDED_FINGERPRINT"],
    "connectorVersion": version, "sourceRevision": os.environ["ASA_REVISION_SHA"],
    "game": "ark-ascended", "platform": os.environ["ARK_ASCENDED_PLATFORM"], "revision": os.environ["ARK_ASCENDED_REVISION"],
}
files[f"{PLUGIN}/takaro-target.json"] = (json.dumps(identity, sort_keys=True) + "\n").encode()
coordinate = os.environ["ARK_ASCENDED_DEP_ASAAPI_COORDINATE"]
files["THIRD-PARTY-NOTICES.txt"] = (
    f"Everything here except {PLUGIN} is the AsaApi release {coordinate.rsplit(':', 1)[1]}\r\n"
    "from https://github.com/ArkServerApi/AsaApi, unmodified, MIT licence: see ArkApi/LICENSE-AsaApi.txt.\r\n"
    f"{PLUGIN}/TakaroAsa.dll contains fmt (https://github.com/fmtlib/fmt, MIT licence).\r\n").encode()
files["PROVENANCE.txt"] = (
    f"takaro-ark-ascended {version} for ARK: Survival Ascended Steam build {os.environ['ARK_ASCENDED_REVISION']}\r\n"
    f"AsaApi: {coordinate} sha256 {os.environ['ARK_ASCENDED_DEP_ASAAPI_SHA256']}\r\n"
    f"TakaroAsa.dll sha256 {hashlib.sha256(dll).hexdigest()}\r\n").encode()

for name in files:
    if name.rsplit("/", 1)[-1] in ("TakaroConfig.txt", "identity-token", "bans.json", "known-players.json", "takaro.log"):
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
