#!/usr/bin/env bash
# Builds the two Valheim roles -- the dedicated-server plugin and the optional
# graphical-client inventory companion -- for one catalog target, and packages each as its
# own deterministic archive named by the target record.
#
# By default the compile itself happens inside the .NET SDK image the target pins, so the
# bytes do not depend on which SDK the caller happens to have: the script prepares the
# pinned inputs on the host and then re-execs itself inside that image. Set
# VALHEIM_BUILD_TOOLCHAIN=host to build in place (needs dotnet, zip, unzip, jq, rg, file).
#
# Usage: build-release.sh <version> <out-dir> [--target <catalog target id>]
set -euo pipefail
cd "$(dirname "$0")/.."

SCRIPT_DIR="$(pwd)/scripts"
REPO_ROOT="$(cd ../.. && pwd)"

VERSION="${1:?usage: build-release.sh <version> <out-dir> [--target <id>]}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir> [--target <id>]}"

# The version is validated before anything else happens: it reaches msbuild properties, a
# JSON document and an archive name, and a rejected build must not have created an output
# directory, resolved a target or touched the network.
source scripts/release-version.sh

if resolve_valheim_release_version "$VERSION"; then
  :
else
  resolution_status=$?
  if [ "$resolution_status" -eq 2 ]; then
    echo "Unsupported semantic version: $VERSION" >&2
    echo "Major, minor, and patch cannot exceed 65534 because BepInEx and .NET assembly metadata require bounded numeric components." >&2
  else
    echo "Invalid semantic version: $VERSION" >&2
    echo "Expected SemVer such as 1.2.3, 1.2.3-rc.1, or 1.2.3+build.4." >&2
  fi
  exit 2
fi

shift 2
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"
valheim_parse_target_flag "$@"
valheim_resolve_target "${TARGET}"

# `VALHEIM_BEPINEX_VERSION` above is the connector's own numeric core -- what BepInEx reads
# as the *plugin* version. The pack and the loader are separate facts with separate names.
BEPINEX_PACK_VERSION="${VALHEIM_BEPINEX_PACK_VERSION:?the resolved target must pin a BepInExPack version}"

DATA_DIR="${VALHEIM_DATA_DIR:-_data}"
VALHEIM_REFERENCE_PATH="${VALHEIM_REFERENCE_PATH:-${DATA_DIR}/references/${VALHEIM_FP16}/valheim_server_Data/Managed}"
BEPINEX_REFERENCE_PATH="${BEPINEX_REFERENCE_PATH:-${DATA_DIR}/deps/bepinex/${VALHEIM_FP16}/BepInExPack_Valheim/BepInEx/core}"
LOADER_VERSION_FILE="${DATA_DIR}/deps/bepinex/${VALHEIM_FP16}/.takaro/loader-version"

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd -- "$OUT_DIR" && pwd)"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$REPO_ROOT" log -1 --format=%ct 2>/dev/null || echo 315532800)}"
if ! [[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || [ "$SOURCE_DATE_EPOCH" -lt 315532800 ]; then
  echo "SOURCE_DATE_EPOCH must be an integer Unix timestamp at or after 1980-01-01." >&2
  exit 2
fi
export TAKARO_SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)}"

VALHEIM_BUILD_TOOLCHAIN="${VALHEIM_BUILD_TOOLCHAIN:-container}"
if [ "$VALHEIM_BUILD_TOOLCHAIN" = "container" ] && [ -z "${VALHEIM_IN_TOOLCHAIN:-}" ]; then
  # The inputs are prepared on the host, where DepotDownloader and curl live; the image
  # only ever sees files that are already hash-checked.
  "${SCRIPT_DIR}/setup-environment.sh" --target "${VALHEIM_TARGET}"

  # The resolved target is handed over as environment rather than resolved again inside:
  # the SDK image has no Python, and re-resolving could pick up a different record.
  toolchain_env=()
  for resolved_key in "${!VALHEIM_@}"; do
    toolchain_env+=(-e "$resolved_key")
  done
  echo "Building ${VALHEIM_TARGET} inside ${VALHEIM_TOOLCHAIN}..."
  docker compose -f docker-compose.yml run --rm --build \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e VALHEIM_IN_TOOLCHAIN=1 \
    -e VALHEIM_BUILD_TOOLCHAIN=host \
    -e SOURCE_DATE_EPOCH \
    -e TAKARO_SOURCE_REVISION \
    "${toolchain_env[@]}" \
    -v "${OUT_DIR}:/out" \
    builder ./scripts/build-release.sh "$VERSION" /out --target "${VALHEIM_TARGET}"
  exit $?
fi

for path in "$VALHEIM_REFERENCE_PATH" "$BEPINEX_REFERENCE_PATH"; do
  [ -d "$path" ] || {
    echo "Missing reference path: $path" >&2
    echo "Run games/valheim/scripts/setup-environment.sh --target ${VALHEIM_TARGET} or set VALHEIM_REFERENCE_PATH and BEPINEX_REFERENCE_PATH." >&2
    exit 1
  }
done

VALHEIM_REFERENCE_PATH="$(realpath "$VALHEIM_REFERENCE_PATH")"
BEPINEX_REFERENCE_PATH="$(realpath "$BEPINEX_REFERENCE_PATH")"
BEPINEX_LOADER_VERSION="$(cat "$LOADER_VERSION_FILE" 2>/dev/null || echo unknown)"
if ! [[ "$BEPINEX_LOADER_VERSION" =~ ^[0-9] ]]; then
  # setup-environment.sh runs wherever the caller is, and reading an assembly's version
  # needs the SDK. This is past the toolchain re-exec, so the SDK is here now: record the
  # loader version the packaged manifest is about to state rather than ship "unknown".
  mkdir -p "$(dirname "$LOADER_VERSION_FILE")"
  # The SDK writes unrelated chatter ("An issue was encountered verifying workloads") to
  # stdout, so the version is picked out by shape rather than by being the only line.
  BEPINEX_LOADER_VERSION="$(dotnet msbuild "${SCRIPT_DIR}/bepinex-loader-version.proj" \
    -nologo -verbosity:minimal \
    -p:BepInExReferencePath="$BEPINEX_REFERENCE_PATH" 2>/dev/null \
    | grep -oE '^[[:space:]]*[0-9]+(\.[0-9]+){1,3}[[:space:]]*$' | tail -n 1 | tr -d '[:space:]' || true)"
  if ! [[ "$BEPINEX_LOADER_VERSION" =~ ^[0-9] ]]; then
    echo "could not read the BepInEx loader version from $BEPINEX_REFERENCE_PATH/BepInEx.dll" >&2
    exit 1
  fi
  printf '%s\n' "$BEPINEX_LOADER_VERSION" > "$LOADER_VERSION_FILE"
fi
echo "  BepInEx loader: ${BEPINEX_LOADER_VERSION} (pack ${BEPINEX_PACK_VERSION})"

SERVER_ARCHIVE="${VALHEIM_ARTIFACT_SERVER_PLUGIN/\{version\}/${VALHEIM_RELEASE_VERSION}}"
COMPANION_ARCHIVE="${VALHEIM_ARTIFACT_CLIENT_COMPANION/\{version\}/${VALHEIM_RELEASE_VERSION}}"

echo "Building Valheim connector and inventory companion v${VALHEIM_RELEASE_VERSION} for ${VALHEIM_TARGET} (${VALHEIM_FP16})..."

# A release is built from nothing but the sources and the pinned inputs: msbuild's
# intermediate output decides what is copied into the package, so it never carries over
# from an earlier build of another target or version.
rm -rf ./mod/obj ./mod/bin
find ./mod -type d \( -name obj -o -name bin \) -prune -exec rm -rf {} + 2>/dev/null || true

dotnet restore mod/Takaro.Valheim.sln

verify_pinned_nupkg() {
  # The two direct NuGet packages, by the sha256 the catalog records. The restore has just
  # run, so the package is on disk under its own name; the archive is found by that name
  # rather than by guessing the layout of the global packages folder.
  local url="$1" expected="$2" name found
  name="$(basename "$url")"
  found="$(find "${NUGET_PACKAGES:-$HOME/.nuget/packages}" -type f -name "$name" -print -quit 2>/dev/null || true)"
  if [ -z "$found" ]; then
    echo "restore did not place $name in the NuGet packages folder; cannot verify it against the pin" >&2
    exit 5
  fi
  if ! printf '%s  %s\n' "$expected" "$found" | sha256sum --check --status; then
    echo "error: $found is not the $name that ${VALHEIM_TARGET} pins" >&2
    exit 5
  fi
  echo "  verified $name"
}

verify_pinned_nupkg "$VALHEIM_DEP_SYSTEM_TEXT_JSON_URL" "$VALHEIM_DEP_SYSTEM_TEXT_JSON_SHA256"
verify_pinned_nupkg \
  "$VALHEIM_DEP_MICROSOFT_NETFRAMEWORK_REFERENCEASSEMBLIES_URL" \
  "$VALHEIM_DEP_MICROSOFT_NETFRAMEWORK_REFERENCEASSEMBLIES_SHA256"

dotnet test mod/Takaro.Valheim.sln --no-restore -v minimal

SERVER_PUBLISH="$(mktemp -d)"
CLIENT_PUBLISH="$(mktemp -d)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$SERVER_PUBLISH" "$CLIENT_PUBLISH" "$STAGE"' EXIT

dotnet publish mod/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -c Release \
  -f net472 \
  --no-restore \
  -o "$SERVER_PUBLISH" \
  -p:EnableValheimPluginBuild=true \
  -p:BepInExReferencePath="$BEPINEX_REFERENCE_PATH" \
  -p:ValheimReferencePath="$VALHEIM_REFERENCE_PATH" \
  -p:TakaroValheimReleaseVersion="$VALHEIM_RELEASE_VERSION" \
  -p:TakaroValheimBepInExVersion="$VALHEIM_BEPINEX_VERSION" \
  -p:Version="$VALHEIM_RELEASE_VERSION" \
  -p:PackageVersion="$VALHEIM_RELEASE_VERSION" \
  -p:AssemblyVersion="$VALHEIM_ASSEMBLY_VERSION" \
  -p:FileVersion="$VALHEIM_ASSEMBLY_VERSION" \
  -p:InformationalVersion="$VALHEIM_RELEASE_VERSION" \
  -p:IncludeSourceRevisionInInformationalVersion=false \
  -p:ContinuousIntegrationBuild=true \
  -p:Deterministic=true \
  -p:PathMap="$(pwd)=/src"

dotnet publish mod/src/Takaro.Valheim.Companion/Takaro.Valheim.Companion.csproj \
  -c Release \
  -f net472 \
  --no-restore \
  -o "$CLIENT_PUBLISH" \
  -p:EnableValheimCompanionBuild=true \
  -p:BepInExReferencePath="$BEPINEX_REFERENCE_PATH" \
  -p:ValheimReferencePath="$VALHEIM_REFERENCE_PATH" \
  -p:TakaroValheimCompanionReleaseVersion="$VALHEIM_RELEASE_VERSION" \
  -p:TakaroValheimCompanionBepInExVersion="$VALHEIM_BEPINEX_VERSION" \
  -p:Version="$VALHEIM_RELEASE_VERSION" \
  -p:PackageVersion="$VALHEIM_RELEASE_VERSION" \
  -p:AssemblyVersion="$VALHEIM_ASSEMBLY_VERSION" \
  -p:FileVersion="$VALHEIM_ASSEMBLY_VERSION" \
  -p:InformationalVersion="$VALHEIM_RELEASE_VERSION" \
  -p:IncludeSourceRevisionInInformationalVersion=false \
  -p:ContinuousIntegrationBuild=true \
  -p:Deterministic=true \
  -p:PathMap="$(pwd)=/src"

SERVER_DIR="$STAGE/TakaroValheim"
CLIENT_DIR="$STAGE/TakaroValheimInventoryCompanion"
mkdir -p "$SERVER_DIR" "$CLIENT_DIR"
cp "$SERVER_PUBLISH"/*.dll "$SERVER_DIR/"
cp "$CLIENT_PUBLISH"/*.dll "$CLIENT_DIR/"

strip_host_assemblies() {
  local package_dir="$1"
  rm -f \
    "$package_dir/0Harmony.dll" \
    "$package_dir/BepInEx.dll" \
    "$package_dir/assembly_valheim.dll" \
    "$package_dir/assembly_utils.dll" \
    "$package_dir/Splatform.dll" \
    "$package_dir/UnityEngine.dll" \
    "$package_dir"/UnityEngine.*.dll \
    "$package_dir/Jotunn.dll" \
    "$package_dir/ServerSync.dll"
}
strip_host_assemblies "$SERVER_DIR"
strip_host_assemblies "$CLIENT_DIR"
rm -f "$SERVER_DIR/Takaro.Valheim.Companion.dll"
rm -f "$CLIENT_DIR/TakaroValheim.dll" "$CLIENT_DIR/Takaro.Valheim.Core.dll"

# The config people edit ships in the plugin folder, with an empty token and identity.
cp mod/takaro.cfg "$SERVER_DIR/takaro.cfg"

cat > "$SERVER_DIR/README.txt" << EOF
Takaro Valheim Connector ${VALHEIM_RELEASE_VERSION}

Built against: ${VALHEIM_TARGET} (Valheim ${VALHEIM_REVISION}, BepInExPack ${BEPINEX_PACK_VERSION}).

Dedicated server install:
1. Install BepInExPack Valheim ${BEPINEX_PACK_VERSION} on the dedicated server.
2. Copy TakaroValheim into BepInEx/plugins/TakaroValheim.
3. Open BepInEx/plugins/TakaroValheim/takaro.cfg, paste your Takaro registration token after
   "registrationToken =" and save. Leave identityToken empty; the connector fills it in.
4. Start the server. You can also paste the token while it runs: the connector checks the file
   every few seconds and connects as soon as you save, no restart needed.

The optional inventory companion (takaro-valheim-inventory-companion.zip) is a separate download for players, not for this server.

Upgrade note: after replacing this folder, delete BepInEx/cache/chainloader_typeloader.dat before restarting. Deterministic archive timestamps can otherwise leave cached metadata from a previous same-size DLL. Your token and identity are kept in BepInEx/config/com.takaro.valheim.cfg; you do not need to paste the token again.

This is the dedicated-server plugin; players need no mod to join. Never commit live registration tokens.
EOF

cat > "$CLIENT_DIR/README.txt" << EOF
Takaro Valheim Inventory Companion ${VALHEIM_RELEASE_VERSION}

Built against: ${VALHEIM_TARGET} (Valheim ${VALHEIM_REVISION}, BepInExPack ${BEPINEX_PACK_VERSION}).

Optional. Players install it on their own game:
1. Install BepInExPack Valheim ${BEPINEX_PACK_VERSION} on your Valheim game.
2. Copy TakaroValheimInventoryCompanion into BepInEx/plugins/ (BepInEx/plugins/TakaroValheimInventoryCompanion).
3. Restart Valheim.

It only lets the server's Takaro connector read your inventory. Nothing else needs it, and players without it play normally. It holds no Takaro token or cloud credential.

Upgrade note: after replacing this folder, delete BepInEx/cache/chainloader_typeloader.dat before restarting. Deterministic archive timestamps can otherwise leave cached metadata from a previous same-size DLL.
EOF

# Three different BepInEx-shaped numbers, each named for what it is:
#   pluginVersion    - what [BepInPlugin] declares, i.e. this connector's own numeric core
#   bepInExPack      - the Thunderstore package the target pins
#   bepInExVersion   - the loader assembly's own version, read off the pack's BepInEx.dll
write_manifest() {
  local path="$1" name="$2" role="$3"
  cat > "$path" << EOF
{
  "name": "${name}",
  "productVersion": "${VALHEIM_RELEASE_VERSION}",
  "pluginVersion": "${VALHEIM_BEPINEX_VERSION}",
  "bepInExPack": {
    "namespace": "${VALHEIM_BEPINEX_PACKAGE%%/*}",
    "name": "${VALHEIM_BEPINEX_PACKAGE#*/}",
    "version": "${BEPINEX_PACK_VERSION}"
  },
  "bepInExVersion": "${BEPINEX_LOADER_VERSION}",
  "target": {
    "game": "valheim",
    "id": "${VALHEIM_TARGET}",
    "revision": "${VALHEIM_REVISION}",
    "fingerprint": "${VALHEIM_FINGERPRINT}"
  },
  "processRole": "${role}",
  "protocol": { "minimum": 3, "current": 3, "maximum": 3 }
}
EOF
}

write_manifest "$SERVER_DIR/manifest.json" TakaroValheim dedicated-server
write_manifest "$CLIENT_DIR/manifest.json" TakaroValheimInventoryCompanion graphical-client

for required in \
  "$SERVER_DIR/TakaroValheim.dll" \
  "$SERVER_DIR/Takaro.Valheim.Core.dll" \
  "$SERVER_DIR/Takaro.Valheim.Companion.Protocol.dll" \
  "$CLIENT_DIR/Takaro.Valheim.Companion.dll" \
  "$CLIENT_DIR/Takaro.Valheim.Companion.Protocol.dll"; do
  [ -f "$required" ] || {
    echo "Publish output is missing required DLL: $required" >&2
    exit 1
  }
done

# One deterministic recipe for the whole repository: scripts/lib/package.sh normalises the
# staged folder's permissions and timestamps to $SOURCE_DATE_EPOCH, feeds the entries
# through `LC_ALL=C sort` so the order cannot come from readdir, and writes the archive with
# `zip -X` so no uid, gid or extended timestamp reaches it.
if [ -n "${VALHEIM_IN_TOOLCHAIN:-}" ] && [ -f /repo/scripts/lib/package.sh ]; then
  # The repository is mounted read-only at /repo inside the toolchain image.
  # shellcheck source=/dev/null
  . /repo/scripts/lib/package.sh
else
  # shellcheck source=../../../scripts/lib/package.sh
  . "${REPO_ROOT}/scripts/lib/package.sh"
fi

normalize_and_zip() {
  local folder_name="$1" archive_name="$2"
  pkg_zip "$STAGE" "$folder_name" "$OUT_DIR/$archive_name"
}

# The output directory is per target, not per version, so the previous build of another
# version is still sitting in it -- and two archives of one role are ambiguous about which
# bytes a release meant, which is why the packaging check below refuses them. Drop the
# earlier output of exactly these two roles (the names come from the target record, with
# the version wildcarded) before writing this build's.
drop_previous_role_archives() {
  local template="$1" pattern
  pattern="${template/\{version\}/*}"
  find "$OUT_DIR" -maxdepth 1 -type f \
    \( -name "$pattern" -o -name "${pattern}.meta.json" \) \
    ! -name "$SERVER_ARCHIVE" ! -name "${SERVER_ARCHIVE}.meta.json" \
    ! -name "$COMPANION_ARCHIVE" ! -name "${COMPANION_ARCHIVE}.meta.json" \
    -delete
}

drop_previous_role_archives "$VALHEIM_ARTIFACT_SERVER_PLUGIN"
drop_previous_role_archives "$VALHEIM_ARTIFACT_CLIENT_COMPANION"

normalize_and_zip TakaroValheim "$SERVER_ARCHIVE"
normalize_and_zip TakaroValheimInventoryCompanion "$COMPANION_ARCHIVE"

# The identity each artifact carries: `takaro-maint artifact validate` reads these files,
# because a zip has no manifest of its own to stamp.
write_meta() {
  local archive="$1" role="$2"
  cat > "${OUT_DIR}/${archive}.meta.json" <<JSON
{
  "target": "${VALHEIM_TARGET}",
  "fingerprint": "${VALHEIM_FINGERPRINT}",
  "connectorVersion": "${VALHEIM_RELEASE_VERSION}",
  "sourceRevision": "${TAKARO_SOURCE_REVISION}",
  "game": "valheim",
  "platform": "linux",
  "revision": "${VALHEIM_REVISION}",
  "role": "${role}"
}
JSON
}

write_meta "$SERVER_ARCHIVE" server-plugin
write_meta "$COMPANION_ARCHIVE" client-companion

VALHEIM_TARGET_ID="$VALHEIM_TARGET" \
VALHEIM_TARGET_FINGERPRINT="$VALHEIM_FINGERPRINT" \
VALHEIM_BEPINEX_VERSION_EXPECTED="$BEPINEX_PACK_VERSION" \
  bash tests/release-package-behavior.sh "$VALHEIM_RELEASE_VERSION" "$OUT_DIR"

echo "  -> $OUT_DIR/$SERVER_ARCHIVE"
echo "  -> $OUT_DIR/$COMPANION_ARCHIVE"
sha256sum "$OUT_DIR/$SERVER_ARCHIVE" "$OUT_DIR/$COMPANION_ARCHIVE"
