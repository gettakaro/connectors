#!/usr/bin/env bash
# Compiles the connector's world-map region on its own and runs it against the tile
# layout Takaro's player map draws: tile geometry and range, map info, getMapTile argument
# shapes, raster sampling and orientation (north up, east right), and the world key.
#
# The region in mod/TakaroConnector.cs depends only on Newtonsoft.Json, so it compiles as a
# plain console project inside the catalog target's pinned .NET SDK image, against the
# Newtonsoft.Json.dll the pinned Rust build ships.
#
# Usage: run.sh [--target <catalog target id>]
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/../.." && pwd)
# shellcheck source=../../scripts/lib-target.sh
. "${PROJECT_ROOT}/scripts/lib-target.sh"

rust_parse_target_flag "$@"
rust_resolve_target "${TARGET}"

cd "${PROJECT_ROOT}"
GAME_REFS="_data/rust-binaries/${RUST_FP16}"
[ -f "${GAME_REFS}/Newtonsoft.Json.dll" ] || { echo "error: ${GAME_REFS}/Newtonsoft.Json.dll is missing; run ./scripts/setup-environment.sh first" >&2; exit 2; }

WORK="_data/map-tests/${RUST_FP16}"
rm -rf "${WORK}"
mkdir -p "${WORK}"

{
    echo 'using System;'
    echo 'using System.Globalization;'
    echo 'using Newtonsoft.Json.Linq;'
    echo
    echo 'internal static class Map'
    echo '{'
    sed -n '/\/\/ takaro:map-begin/,/\/\/ takaro:map-end/p' mod/TakaroConnector.cs
    echo '}'
} > "${WORK}/Map.cs"
grep -q 'takaro:map-begin' "${WORK}/Map.cs" || { echo "error: the map region markers are gone from mod/TakaroConnector.cs" >&2; exit 5; }

cp tests/map/Program.cs "${WORK}/Program.cs"

cat > "${WORK}/Map.Tests.csproj" <<CSPROJ
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <Nullable>disable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
    <InvariantGlobalization>true</InvariantGlobalization>
    <EnableDefaultItems>false</EnableDefaultItems>
    <RestorePackages>false</RestorePackages>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="Map.cs" />
    <Compile Include="Program.cs" />
    <Reference Include="Newtonsoft.Json" HintPath="/work/games/rust/${GAME_REFS}/Newtonsoft.Json.dll" />
  </ItemGroup>
</Project>
CSPROJ

REPO_ROOT="$(rust_repo_root)"
echo "Running the map harness for ${RUST_TARGET} in ${RUST_TOOLCHAIN}..."
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e DOTNET_CLI_HOME=/tmp \
    -e DOTNET_NOLOGO=1 \
    -e DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    -v "${REPO_ROOT}:/work" \
    -w "/work/games/rust/${WORK}" \
    "${RUST_TOOLCHAIN}" \
    dotnet run -c Release

echo "World map tiles pass for ${RUST_TARGET}"
