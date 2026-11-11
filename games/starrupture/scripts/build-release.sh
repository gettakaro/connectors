#!/usr/bin/env bash
# Builds the StarRupture release zip for one catalog target:
#   takaro-starrupture-<target>-<version>.zip, unpacked by the operator into
#   StarRupture/Binaries/Win64.
#
#   games/starrupture/scripts/build-release.sh <version> <out-dir> [--target <id>]
#
# TakaroStarRupture.dll is cross-compiled with clang-cl and lld-link inside the pinned Debian image,
# against the MSVC CRT and Windows SDK the pinned xwin fetches. The Plugin SDK and Game SDK are build
# inputs only: they are fetched at the pinned commit, checked against the catalog's tree digest and
# never packaged. The zip carries the pinned ModLoader (MIT, its LICENSE included), the plugin and the
# example config.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
repo_root=$(cd -- "$project_root/../.." && pwd)
. "$script_dir/lib-target.sh"

version="${1:?usage: build-release.sh <version> <out-dir> [--target id]}"
out_dir="${2:?usage: build-release.sh <version> <out-dir> [--target id]}"
shift 2
starrupture_parse_target_flag "$@"
starrupture_resolve_target "$TARGET"
case "$version" in *[!A-Za-z0-9._+-]*|"") echo "invalid version '$version'" >&2; exit 2;; esac
mkdir -p "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd)
epoch="${SOURCE_DATE_EPOCH:-$(git -C "$repo_root" log -1 --format=%ct)}"
revision="${TAKARO_SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}"

# The compiler packages, at exactly these versions from the pinned image's own release.
LLVM_VERSION="1:19.1.7-3+b1"
CA_VERSION="20250419"
# MSVC CRT and Windows SDK versions, read from the catalog coordinate
# (microsoft:msvc-crt-<crt>+winsdk:<sdk>).
crt_version="${STARRUPTURE_DEP_MSVC_SYSROOT_COORDINATE#microsoft:msvc-crt-}"
crt_version="${crt_version%%+winsdk:*}"
sdk_version="${STARRUPTURE_DEP_MSVC_SYSROOT_COORDINATE##*+winsdk:}"

cache="${TAKARO_MAINT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/takaro-maint}/starrupture"
mkdir -p "$cache/downloads"

fetch() { # fetch <url> <sha256> <file>
    local url="$1" sha="$2" file="$cache/downloads/$3"
    if [ ! -f "$file" ] || ! echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
        curl -fsSL --retry 3 "$url" -o "$file.part"
        mv "$file.part" "$file"
    fi
    echo "$sha  $file" | sha256sum -c --quiet -
}
fetch "$STARRUPTURE_DEP_XWIN_URL" "$STARRUPTURE_DEP_XWIN_SHA256" xwin.tar.gz
fetch "$STARRUPTURE_DEP_MODLOADER_URL" "$STARRUPTURE_DEP_MODLOADER_SHA256" modloader.zip

tree_digest() { # tree_digest <checkout> <subdir>
    (cd "$1" && find "$2" -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d" " -f1)
}
fetch_sdk() { # fetch_sdk <dir> <coordinate owner:repo:ref> <subdir> <tree sha256>
    local dir="$cache/$1" owner repo ref
    IFS=: read -r owner repo ref <<<"$2"
    if [ ! -d "$dir/.git" ] || [ "$(git -C "$dir" rev-parse HEAD)" != "$ref" ]; then
        rm -rf "$dir"
        git init -q "$dir"
        git -C "$dir" fetch -q --depth 1 "https://github.com/$owner/$repo.git" "$ref"
        git -C "$dir" checkout -q FETCH_HEAD
    fi
    local got
    got=$(tree_digest "$dir" "$3")
    if [ "$got" != "$4" ]; then echo "$1 tree digest $got != catalog $4" >&2; exit 1; fi
}
fetch_sdk pluginSdk "$STARRUPTURE_DEP_PLUGIN_SDK_COORDINATE" include "$STARRUPTURE_DEP_PLUGIN_SDK_SHA256"
fetch_sdk gameSdk "$STARRUPTURE_DEP_GAME_SDK_COORDINATE" Server "$STARRUPTURE_DEP_GAME_SDK_SHA256"

stage="$project_root/_data/build/stage-${STARRUPTURE_FP16}"
rm -rf "$stage"
mkdir -p "$stage/build"

# Compile inside the pinned image (as root for apt; the outputs are handed back to the caller).
docker run --rm -e HOME=/tmp -e SOURCE_DATE_EPOCH="$epoch" \
  -e LLVM_VERSION="$LLVM_VERSION" -e CA_VERSION="$CA_VERSION" \
  -e CRT_VERSION="$crt_version" -e SDK_VERSION="$sdk_version" \
  -e SYSROOT_SHA256="$STARRUPTURE_DEP_MSVC_SYSROOT_SHA256" -e VERSION="$version" \
  -e JOBS="${STARRUPTURE_BUILD_JOBS:-2}" -e OWNER="$(id -u):$(id -g)" \
  -v "$cache:/cache" -v "$project_root/mod:/mod:ro" -v "$stage:/stage" \
  "$STARRUPTURE_TOOLCHAIN" bash -c '
    set -euo pipefail
    trap "chown -R $OWNER /stage /cache" EXIT
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    apt-get install -y -qq --no-install-recommends \
      "clang-19=$LLVM_VERSION" "lld-19=$LLVM_VERSION" "llvm-19=$LLVM_VERSION" "ca-certificates=$CA_VERSION" >/dev/null

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

    echo "== TakaroStarRupture.dll (clang-cl $LLVM_VERSION)"
    game=/cache/gameSdk/Server
    export CL_FLAGS="--target=x86_64-pc-windows-msvc /std:c++20 /O2 /MD /EHsc /GS /W0 /utf-8 /Brepro -Wno-everything
      /DNDEBUG /D_WINDOWS /D_USRDLL /DTAKAROSTARRUPTURE_EXPORTS /DTAKARO_STARRUPTURE_WITH_GAME_SDK=1
      -imsvc $sysroot/crt/include -imsvc $sysroot/sdk/include/ucrt -imsvc $sysroot/sdk/include/um
      -imsvc $sysroot/sdk/include/shared -I/cache/pluginSdk/include -I$game -I$game/SDK -I/mod/src"
    { find /mod/src -name "*.cpp" | LC_ALL=C sort
      for unit in Basic CoreUObject_functions Engine_functions AuItems_functions Chimera_functions; do
        echo "$game/SDK/$unit.cpp"
      done
    } | xargs -P "$JOBS" -I{} bash -c '"'"'clang-19 --driver-mode=cl $CL_FLAGS "-DTAKARO_PLUGIN_VERSION=\"$VERSION\"" /c "$1" /Fo"/stage/build/$(basename "$1" .cpp).obj"'"'"' _ {}
    lld-link-19 /DLL /OUT:/stage/build/TakaroStarRupture.dll /MACHINE:X64 /OPT:REF /OPT:ICF /Brepro \
      "/libpath:$sysroot/crt/lib/x86_64" "/libpath:$sysroot/sdk/lib/um/x86_64" "/libpath:$sysroot/sdk/lib/ucrt/x86_64" \
      /stage/build/*.obj dbghelp.lib winhttp.lib crypt32.lib advapi32.lib user32.lib
    exports=$(llvm-readobj-19 --coff-exports /stage/build/TakaroStarRupture.dll | sed -n "s/^ *Name: //p" | LC_ALL=C sort | tr "\n" " ")
    if [ "$exports" != "GetPluginInfo PluginInit PluginShutdown " ]; then
      echo "unexpected DLL exports: $exports" >&2; exit 1
    fi
  '

dll="$stage/build/TakaroStarRupture.dll"
echo "== package"
name="${STARRUPTURE_ARTIFACT_MOD/\{version\}/$version}"
STARRUPTURE_VERSION="$version" STARRUPTURE_EPOCH="$epoch" STARRUPTURE_REVISION_SHA="$revision" \
python3 -I - "$project_root" "$dll" "$out_dir/$name" "$cache/downloads/modloader.zip" <<'PY'
import json, os, sys, time, zipfile
project, dll, dest, loader_zip = sys.argv[1:5]
stamp = time.gmtime(max(int(os.environ["STARRUPTURE_EPOCH"]), 315532800))[:6]
PLUGINS = "ModLoader/Plugins"

files = {}
with zipfile.ZipFile(loader_zip) as loader:
    for entry in ("dwmapi.dll", "ModLoader/StarRupture-ModLoader-Core.dll", "ModLoader/StarRupture-ModLoader-Core.pdb",
                  "ModLoader/readme.txt"):
        files[entry] = loader.read(entry)
files[f"{PLUGINS}/TakaroStarRupture.dll"] = open(dll, "rb").read()
files[f"{PLUGINS}/config/TakaroStarRupture.example.ini"] = open(
    os.path.join(project, "mod", "TakaroStarRupture.example.ini"), "rb").read()
files["THIRD-PARTY-NOTICES.txt"] = open(os.path.join(project, "mod", "THIRD_PARTY_NOTICES.md"), "rb").read()
files["PROVENANCE.txt"] = (
    f"takaro-starrupture {os.environ['STARRUPTURE_VERSION']}\n"
    f"source {os.environ['STARRUPTURE_REVISION_SHA']}\n"
    f"target {os.environ['STARRUPTURE_TARGET']}\n"
    f"modloader {os.environ['STARRUPTURE_DEP_MODLOADER_COORDINATE']}\n").encode()

with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as archive:
    for name in sorted(files):
        info = zipfile.ZipInfo(name, stamp)
        info.external_attr = 0o100644 << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, files[name])
meta = {"target": os.environ["STARRUPTURE_TARGET"], "fingerprint": os.environ["STARRUPTURE_FINGERPRINT"],
        "connectorVersion": os.environ["STARRUPTURE_VERSION"], "sourceRevision": os.environ["STARRUPTURE_REVISION_SHA"],
        "game": "starrupture", "platform": "windows", "revision": os.environ["STARRUPTURE_REVISION"]}
open(dest + ".meta.json", "w").write(json.dumps(meta, sort_keys=True) + "\n")
PY
sha256sum "$dll" "$out_dir/$name"
