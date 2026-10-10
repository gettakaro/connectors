#!/usr/bin/env bash
# Compiles the connector's config region on its own and runs it against the install,
# upgrade and edit-while-running cases: fresh install, existing environment install,
# upgrade with an existing file, environment precedence, a half-saved file, identity
# generation and preservation.
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

WORK="_data/config-tests/${RUST_FP16}"
rm -rf "${WORK}"
mkdir -p "${WORK}"

{
    echo 'using System;'
    echo 'using System.Collections.Generic;'
    echo 'using Newtonsoft.Json;'
    echo 'using Newtonsoft.Json.Linq;'
    echo
    echo 'internal static class Config'
    echo '{'
    sed -n '/\/\/ takaro:config-begin/,/\/\/ takaro:config-end/p' mod/TakaroConnector.cs
    echo '}'
} > "${WORK}/Config.cs"
grep -q 'takaro:config-begin' "${WORK}/Config.cs" || { echo "error: the config region markers are gone from mod/TakaroConnector.cs" >&2; exit 5; }

cp tests/config/Program.cs "${WORK}/Program.cs"

cat > "${WORK}/Config.Tests.csproj" <<CSPROJ
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
    <Compile Include="Config.cs" />
    <Compile Include="Program.cs" />
    <Reference Include="Newtonsoft.Json" HintPath="/work/games/rust/${GAME_REFS}/Newtonsoft.Json.dll" />
  </ItemGroup>
</Project>
CSPROJ

REPO_ROOT="$(rust_repo_root)"
echo "Running the config harness for ${RUST_TARGET} in ${RUST_TOOLCHAIN}..."
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

echo "Config resolution passes for ${RUST_TARGET}"
