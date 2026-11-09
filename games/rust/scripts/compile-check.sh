#!/usr/bin/env bash
# Compiles mod/TakaroConnector.cs against the pinned game and Carbon assemblies, inside
# the catalog target's pinned .NET SDK image. This is the authoritative build for the
# fingerprint: the released artifact is source, so nothing else proves it compiles.
#
# Usage: compile-check.sh [--target <catalog target id>]
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

rust_parse_target_flag "$@"
rust_resolve_target "${TARGET}"

cd "${PROJECT_ROOT}"
GAME_REFS="_data/rust-binaries/${RUST_REFERENCES_KEY}"
CARBON_REFS="_data/carbon-refs/${RUST_FP16}"
WORK="_data/compile/${RUST_FP16}"

for dir in "${GAME_REFS}" "${CARBON_REFS}"; do
    [ -d "${dir}" ] || { echo "error: ${dir} is missing; run ./scripts/setup-environment.sh first" >&2; exit 2; }
done

rm -rf "${WORK}"
mkdir -p "${WORK}" "_data/nuget"
cp compile/TakaroConnector.Compile.csproj "${WORK}/TakaroConnector.Compile.csproj"
cp compile/packages.lock.json "${WORK}/packages.lock.json"
cp mod/TakaroConnector.cs "${WORK}/TakaroConnector.cs"

# One <Reference> per pinned assembly. The game's are publicized (Rust's plugin API is
# mostly internal); Carbon's are referenced as they ship.
{
    echo '<Project>'
    echo '  <ItemGroup>'
    for dll in "${GAME_REFS}"/*.dll; do
        name=$(basename "${dll}" .dll)
        case "${name}" in System.*|mscorlib|netstandard|Microsoft.*) continue ;; esac
        printf '    <Reference Include="%s" HintPath="/work/games/rust/%s/%s.dll" Publicize="true" />\n' \
            "${name}" "${GAME_REFS}" "${name}"
    done
    for dll in "${CARBON_REFS}"/*.dll; do
        name=$(basename "${dll}" .dll)
        printf '    <Reference Include="%s" HintPath="/work/games/rust/%s/%s.dll" />\n' \
            "${name}" "${CARBON_REFS}" "${name}"
    done
    echo '  </ItemGroup>'
    echo '</Project>'
} > "${WORK}/references.props"

REPO_ROOT="$(rust_repo_root)"
echo "Compiling TakaroConnector against ${RUST_TARGET} in ${RUST_TOOLCHAIN}..."
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
    dotnet build -c Release -p:RestoreLockedMode=true

# The restored package is checked after the fact: --locked-mode pins the version, the
# catalog pins the bytes.
NUPKG="_data/nuget/bepinex.assemblypublicizer.msbuild/0.4.2/bepinex.assemblypublicizer.msbuild.0.4.2.nupkg"
[ -f "${NUPKG}" ] || { echo "error: ${NUPKG} was not restored" >&2; exit 5; }
printf '%s  %s\n' "${RUST_DEP_BEPINEX_ASSEMBLYPUBLICIZER_MSBUILD_SHA256}" "${NUPKG}" \
    | sha256sum --check --status \
    || { echo "error: ${NUPKG} is not the package ${RUST_TARGET} pins" >&2; exit 5; }
echo "publicizer nupkg matches ${RUST_DEP_BEPINEX_ASSEMBLYPUBLICIZER_MSBUILD_SHA256}"

echo "Compile-check passed for ${RUST_TARGET}"
