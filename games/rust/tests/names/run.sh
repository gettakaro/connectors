#!/usr/bin/env bash
# Compiles the connector's entity-name and frame-summary regions on their own, then runs
# them against the vectors, the prefab list a real server returned and secret-bearing frames.
#
# The marked regions in mod/TakaroConnector.cs have no Rust, Carbon or Unity dependency,
# so they compile as a
# plain console project inside the catalog target's pinned .NET SDK image -- which means
# the names Takaro shows are asserted by running the shipped code, not by grepping it.
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
WORK="_data/names/${RUST_FP16}"
rm -rf "${WORK}"
mkdir -p "${WORK}" "_data/nuget"

# The region, lifted out of the shipped source and wrapped in a type of its own.
{
    echo 'using System;'
    echo 'using System.Collections.Generic;'
    echo 'using System.Text;'
    echo 'using Newtonsoft.Json.Linq;'
    echo
    echo 'internal static class Names'
    echo '{'
    sed -n '/\/\/ takaro:names-begin/,/\/\/ takaro:names-end/p' mod/TakaroConnector.cs
    sed -n '/\/\/ takaro:frames-begin/,/\/\/ takaro:frames-end/p' mod/TakaroConnector.cs
    sed -n '/\/\/ takaro:identity-begin/,/\/\/ takaro:identity-end/p' mod/TakaroConnector.cs
    echo '    public static string EntityDisplayName(string shortName)'
    echo '    {'
    echo '        return EntityNames.EntityDisplayName(shortName);'
    echo '    }'
    echo '    public static bool IsPlayerId(string id)'
    echo '    {'
    echo '        return IsSteamPlayerId(id);'
    echo '    }'
    echo '    public static string[][] Identity(string steamId)'
    echo '    {'
    echo '        return IdentityFields(steamId);'
    echo '    }'
    echo '    public static string SummarizeFrame(string message)'
    echo '    {'
    echo '        return FrameSummary(message);'
    echo '    }'
    echo '}'
} > "${WORK}/Names.cs"
grep -q 'takaro:names-begin' "${WORK}/Names.cs" || { echo "error: the name region markers are gone from mod/TakaroConnector.cs" >&2; exit 5; }
grep -q 'takaro:frames-begin' "${WORK}/Names.cs" || { echo "error: the frame region markers are gone from mod/TakaroConnector.cs" >&2; exit 5; }
grep -q 'takaro:identity-begin' "${WORK}/Names.cs" || { echo "error: the identity region markers are gone from mod/TakaroConnector.cs" >&2; exit 5; }

cp tests/names/Program.cs "${WORK}/Program.cs"
cp tests/names/NewtonsoftStub.cs "${WORK}/NewtonsoftStub.cs"
cp tests/names/names.tsv tests/names/entity-codes.txt "${WORK}/"

cat > "${WORK}/Names.Tests.csproj" <<'CSPROJ'
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
    <Compile Include="Names.cs" />
    <Compile Include="NewtonsoftStub.cs" />
    <Compile Include="Program.cs" />
  </ItemGroup>
</Project>
CSPROJ

REPO_ROOT="$(rust_repo_root)"
echo "Running the entity-name harness for ${RUST_TARGET} in ${RUST_TOOLCHAIN}..."
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e DOTNET_CLI_HOME=/tmp \
    -e DOTNET_NOLOGO=1 \
    -e DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    -e NUGET_PACKAGES=/work/games/rust/_data/nuget \
    -v "${REPO_ROOT}:/work" \
    -w "/work/games/rust/${WORK}" \
    "${RUST_TOOLCHAIN}" \
    dotnet run -c Release -- .

echo "Entity names pass for ${RUST_TARGET}"
