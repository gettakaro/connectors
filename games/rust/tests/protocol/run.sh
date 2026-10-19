#!/usr/bin/env bash
# Compiles the connector's protocol region (with the config region it reads identify errors
# with) on its own and runs it against Generic Game Protocol v0 and v1: the identify offer and
# Capability Manifest, identifyResponse in both versions, response and error frames, and the
# numbered event stream with acks, resend and overflow.
#
# The regions in mod/TakaroConnector.cs depend only on Newtonsoft.Json, so they compile as a
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

WORK="_data/protocol-tests/${RUST_FP16}"
rm -rf "${WORK}"
mkdir -p "${WORK}"

{
    echo 'using System;'
    echo 'using System.Collections.Generic;'
    echo 'using System.Linq;'
    echo 'using Newtonsoft.Json;'
    echo 'using Newtonsoft.Json.Linq;'
    echo
    echo 'internal static class Connector'
    echo '{'
    sed -n '/\/\/ takaro:config-begin/,/\/\/ takaro:config-end/p' mod/TakaroConnector.cs
    sed -n '/\/\/ takaro:protocol-begin/,/\/\/ takaro:protocol-end/p' mod/TakaroConnector.cs
    echo '}'
} > "${WORK}/Connector.cs"
grep -q 'takaro:protocol-begin' "${WORK}/Connector.cs" || { echo "error: the protocol region markers are gone from mod/TakaroConnector.cs" >&2; exit 5; }

cp tests/protocol/Program.cs "${WORK}/Program.cs"

cat > "${WORK}/Protocol.Tests.csproj" <<CSPROJ
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
    <Compile Include="Connector.cs" />
    <Compile Include="Program.cs" />
    <Reference Include="Newtonsoft.Json" HintPath="/work/games/rust/${GAME_REFS}/Newtonsoft.Json.dll" />
  </ItemGroup>
</Project>
CSPROJ

REPO_ROOT="$(rust_repo_root)"
echo "Running the protocol harness for ${RUST_TARGET} in ${RUST_TOOLCHAIN}..."
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

echo "Protocol checks pass for ${RUST_TARGET}"
