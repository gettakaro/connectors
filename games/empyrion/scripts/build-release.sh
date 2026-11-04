#!/usr/bin/env bash
# Builds the Empyrion release zip for one catalog target:
#   takaro-empyrion-<target>-<version>.zip, unpacked by the operator into <server>/Content/Mods.
#
#   games/empyrion/scripts/build-release.sh <version> <out-dir> [--target <id>]
#
# TakaroEmpyrion.dll is compiled with the Mono C# compiler of the pinned mono image against the
# three server assemblies the target pins (Mif.dll, ModApi.dll, Newtonsoft.Json.dll), fetched from
# the pinned depot manifest by `takaro-maint steam references` and checked by sha256, plus the
# image's netstandard facade, also checked by sha256. EMPYRION_REFS_DIR=<dir> uses a directory that
# already holds the three assemblies instead (still hash-checked). The contract tests run against
# the same sources before anything is packaged. Building twice gives the same bytes.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
repo_root=$(cd -- "$project_root/../.." && pwd)
# shellcheck source=lib-target.sh
. "$script_dir/lib-target.sh"

version="${1:?usage: build-release.sh <version> <out-dir> [--target id]}"
out_dir="${2:?usage: build-release.sh <version> <out-dir> [--target id]}"
shift 2
empyrion_parse_target_flag "$@"
empyrion_resolve_target "$TARGET"
case "$version" in *[!A-Za-z0-9._+-]*|"") echo "invalid version '$version'" >&2; exit 2;; esac
mkdir -p "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd)
epoch="${SOURCE_DATE_EPOCH:-$(git -C "$repo_root" log -1 --format=%ct)}"
revision="${TAKARO_SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}"

echo "== reference assemblies"
refs="${EMPYRION_REFS_DIR:-$project_root/_data/refs/$EMPYRION_FP16}"
if [ -n "${EMPYRION_REFS_DIR:-}" ]; then
    empyrion_check_references "$refs"
else
    empyrion_references "$refs"
fi
refs=$(cd -- "$refs" && pwd)

# The facade's path inside the image comes from the catalog coordinate (image-file:<tag>:<path>).
facade="${EMPYRION_DEP_NETSTANDARD_DLL_COORDINATE#image-file:*:}"

stage="$project_root/_data/build/stage-${EMPYRION_FP16}"
rm -rf "$stage"
mkdir -p "$stage/out"
cp -r "$project_root/mod" "$project_root/tests" "$project_root/capabilities.json" \
    "$project_root/TakaroConfig.txt.example" "$stage/"

# Stamp the release version into the stage copy; the tracked files carry release-please markers.
python3 -I - "$stage" "$version" <<'PY'
import re, sys
stage, version = sys.argv[1:3]
for path, pattern, repl in (
    (f"{stage}/mod/src/TakaroMod.cs", r'public const string Version = "[^"]*";', f'public const string Version = "{version}";'),
    (f"{stage}/mod/TakaroEmpyrion_Info.yaml", r"(?m)^Version: .*$", f"Version: {version}"),
):
    text = open(path, encoding="utf-8").read()
    text, n = re.subn(pattern, lambda _m: repl, text)
    if n != 1:
        sys.exit(f"{path}: expected exactly one version line, found {n}")
    if path.endswith(".yaml"):
        # The release-please markers are for the tracked file; the shipped one is the plain file
        # the game's mod loader was proven with.
        text = re.sub(r"(?m)^# x-release-please-(?:start-version|end)\r?\n", "", text)
    open(path, "w", encoding="utf-8", newline="").write(text)
PY

echo "== compile and contract tests ($EMPYRION_TOOLCHAIN)"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e SOURCE_DATE_EPOCH="$epoch" \
  -e FACADE="$facade" -e FACADE_SHA256="$EMPYRION_DEP_NETSTANDARD_DLL_SHA256" \
  -v "$stage:/stage" -v "$refs:/refs:ro" -w /stage \
  "$EMPYRION_TOOLCHAIN" bash -c '
    set -euo pipefail
    echo "$FACADE_SHA256  $FACADE" | sha256sum --check --quiet -
    refs=(-r:/refs/Mif.dll -r:/refs/ModApi.dll -r:/refs/Newtonsoft.Json.dll -r:"$FACADE"
          -r:System.Net.Http.dll -r:System.IO.Compression.dll)
    mapfile -t sources < <(find mod/src -name "*.cs" | LC_ALL=C sort)
    mapfile -t tests < <(find tests -name "*.cs" | LC_ALL=C sort)
    csc -nologo -langversion:7.3 -optimize+ -deterministic -debug- -pathmap:/stage=/takaro -target:library \
      -out:out/TakaroEmpyrion.dll "${refs[@]}" "${sources[@]}"
    mkdir -p out/tests
    cp /refs/Mif.dll /refs/ModApi.dll /refs/Newtonsoft.Json.dll out/tests/
    csc -nologo -langversion:7.3 -target:exe -out:out/tests/contract-tests.exe "${refs[@]}" "${sources[@]}" "${tests[@]}"
    mono out/tests/contract-tests.exe capabilities.json
  '

echo "== package"
name="${EMPYRION_ARTIFACT_MOD/\{version\}/$version}"
EMPYRION_VERSION="$version" EMPYRION_EPOCH="$epoch" EMPYRION_REVISION_SHA="$revision" \
python3 -I - "$stage" "$out_dir/$name" <<'PY'
import hashlib, json, os, sys, time, zipfile
stage, dest = sys.argv[1:3]
version = os.environ["EMPYRION_VERSION"]
stamp = time.gmtime(max(int(os.environ["EMPYRION_EPOCH"]), 315532800))[:6]
MOD = "TakaroEmpyrion"

files = {
    f"{MOD}/TakaroEmpyrion.dll": open(os.path.join(stage, "out", "TakaroEmpyrion.dll"), "rb").read(),
    f"{MOD}/TakaroEmpyrion_Info.yaml": open(os.path.join(stage, "mod", "TakaroEmpyrion_Info.yaml"), "rb").read(),
    f"{MOD}/TakaroConfig.txt.example": open(os.path.join(stage, "TakaroConfig.txt.example"), "rb").read(),
}
identity = {
    "target": os.environ["EMPYRION_TARGET"], "fingerprint": os.environ["EMPYRION_FINGERPRINT"],
    "connectorVersion": version, "sourceRevision": os.environ["EMPYRION_REVISION_SHA"],
    "game": "empyrion", "platform": os.environ["EMPYRION_PLATFORM"], "revision": os.environ["EMPYRION_REVISION"],
}
files[f"{MOD}/takaro-target.json"] = (json.dumps(identity, sort_keys=True) + "\n").encode()
references = "".join(f"  {entry.replace(':', ' sha256 ', 1)}\r\n" for entry in os.environ["EMPYRION_REFERENCES"].split(";"))
files[f"{MOD}/PROVENANCE.txt"] = (
    f"takaro-empyrion {version} for Empyrion - Galactic Survival Steam build {os.environ['EMPYRION_REVISION']}\r\n"
    f"TakaroEmpyrion.dll sha256 {hashlib.sha256(files[f'{MOD}/TakaroEmpyrion.dll']).hexdigest()}\r\n"
    f"compiled with csc from {os.environ['EMPYRION_TOOLCHAIN']} against:\r\n{references}").encode()

for name in files:
    parts = name.split("/")
    if parts[1] in ("TakaroConfig.txt", "players.json", "bans.json", "Logs"):
        sys.exit(f"refusing to package operator state {name}")
with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in sorted(files):
        entry = zipfile.ZipInfo(name, stamp)
        entry.compress_type = zipfile.ZIP_DEFLATED
        entry.external_attr = (0o100644 << 16)
        z.writestr(entry, files[name], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
open(dest + ".meta.json", "w").write(json.dumps(identity, sort_keys=True) + "\n")
PY
sha256sum "$stage/out/TakaroEmpyrion.dll" "$out_dir/$name"
