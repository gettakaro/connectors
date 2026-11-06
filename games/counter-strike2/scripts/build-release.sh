#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
repo_root=$(cd -- "$project_root/../.." && pwd)
. "$script_dir/lib-target.sh"

version=${1:?usage: build-release.sh <version> <out-dir> --target <id>}
out_dir=${2:?usage: build-release.sh <version> <out-dir> --target <id>}
shift 2
cs2_parse_target_flag "$@"
cs2_resolve_target "$TARGET"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([+.-][A-Za-z0-9.-]+)?$ ]] || { echo "invalid version" >&2; exit 2; }

mkdir -p "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd)
artifact="takaro-counter-strike2-${CS2_TARGET}-${version}.zip"
expected_api_sha=$(jq -r '.build.deps["CounterStrikeSharp.API"].sha256' "$repo_root/catalog/counter-strike2/targets/$CS2_TARGET.json")
expected_stb_read_sha=$(jq -r '.build.deps.StbImageSharp.sha256' "$repo_root/catalog/counter-strike2/targets/$CS2_TARGET.json")
expected_stb_write_sha=$(jq -r '.build.deps.StbImageWriteSharp.sha256' "$repo_root/catalog/counter-strike2/targets/$CS2_TARGET.json")
api_version=$(jq -r '.build.deps["CounterStrikeSharp.API"].coordinate | split(":")[2]' "$repo_root/catalog/counter-strike2/targets/$CS2_TARGET.json")
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

docker run --rm -e EXPECTED_API_SHA="$expected_api_sha" -e EXPECTED_STB_READ_SHA="$expected_stb_read_sha" -e EXPECTED_STB_WRITE_SHA="$expected_stb_write_sha" -e CS2_API_VERSION="$api_version" -v "$repo_root:/repo" -w /repo/games/counter-strike2/mod \
  mcr.microsoft.com/dotnet/sdk:10.0@sha256:0d99ebf1ccad15037b5c98c9fe8979d6b8dbac23812feae2277a0b277b5c5ffa \
  bash -lc 'set -euo pipefail
    dotnet restore src/Takaro.Cs2.Plugin/Takaro.Cs2.Plugin.csproj --locked-mode --nologo
    dotnet build src/Takaro.Cs2.Plugin/Takaro.Cs2.Plugin.csproj -c Release --no-restore --nologo
    cp /root/.nuget/packages/stbimagesharp/2.30.16/lib/net8.0/StbImageSharp.dll src/Takaro.Cs2.Plugin/bin/Release/net10.0/
    cp /root/.nuget/packages/stbimagewritesharp/1.16.7/lib/netstandard2.0/StbImageWriteSharp.dll src/Takaro.Cs2.Plugin/bin/Release/net10.0/
    printf "%s  %s\n" "$EXPECTED_API_SHA" "/root/.nuget/packages/counterstrikesharp.api/$CS2_API_VERSION/counterstrikesharp.api.$CS2_API_VERSION.nupkg" | sha256sum -c -
    printf "%s  %s\n" "$EXPECTED_STB_READ_SHA" /root/.nuget/packages/stbimagesharp/2.30.16/stbimagesharp.2.30.16.nupkg | sha256sum -c -
    printf "%s  %s\n" "$EXPECTED_STB_WRITE_SHA" /root/.nuget/packages/stbimagewritesharp/1.16.7/stbimagewritesharp.1.16.7.nupkg | sha256sum -c -'

mkdir -p "$stage/TakaroCs2"
output="$project_root/mod/src/Takaro.Cs2.Plugin/bin/Release/net10.0"
cp "$output/TakaroCs2.dll" "$output/TakaroCs2.deps.json" "$output/Takaro.Cs2.Core.dll" "$output/StbImageSharp.dll" "$output/StbImageWriteSharp.dll" "$stage/TakaroCs2/"
cp "$project_root/TakaroCs2.example.json" "$project_root/README.md" "$stage/TakaroCs2/"
cp "$project_root/scripts/prepare-maps.sh" "$stage/TakaroCs2/"
printf '%s\n' "$version" > "$stage/TakaroCs2/version.txt"
source_date_epoch=${SOURCE_DATE_EPOCH:-$(git -C "$repo_root" log -1 --format=%ct)}
SOURCE_DATE_EPOCH="$source_date_epoch" STAGE="$stage" ARCHIVE="$out_dir/$artifact" python3 - <<'PY'
import datetime, os, pathlib, zipfile
stage = pathlib.Path(os.environ['STAGE'])
stamp = datetime.datetime.fromtimestamp(max(315532800, int(os.environ['SOURCE_DATE_EPOCH'])), datetime.timezone.utc)
date_time = (stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second)
with zipfile.ZipFile(os.environ['ARCHIVE'], 'w') as archive:
    for path in sorted((stage / 'TakaroCs2').rglob('*')):
        if not path.is_file():
            continue
        name = path.relative_to(stage).as_posix()
        info = zipfile.ZipInfo(name, date_time)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        archive.writestr(info, path.read_bytes())
with zipfile.ZipFile(os.environ['ARCHIVE']) as archive:
    assert archive.testzip() is None
PY
cat > "$out_dir/$artifact.meta.json" <<JSON
{"target":"${CS2_TARGET}","fingerprint":"${CS2_FINGERPRINT}","connectorVersion":"${version}","sourceRevision":"${TAKARO_SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}"}
JSON
sha256sum "$out_dir/$artifact"
