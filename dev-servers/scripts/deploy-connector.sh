#!/usr/bin/env bash
# Builds a connector from this working tree using the repo's own build scripts
# and copies the artifact into dev-servers/_data/<game>/.
#
# Usage: deploy-connector.sh <game>
#
# Run this on its own after editing connector code — no reinstall needed.
# Most games then need a restart: dev-servers/scripts/start.sh <game>
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../lib" && pwd)/common.sh"

GAME="${1:?usage: deploy-connector.sh <game>}"
ds_validate_game "$GAME"

deploy_rust() {
    local dest
    dest="$(ds_data_dir rust)/plugins"
    mkdir -p "$dest"
    # Carbon compiles the .cs at runtime — there is no build step.
    cp "${REPO_ROOT}/games/rust/mod/TakaroConnector.cs" "${dest}/TakaroConnector.cs"
    ds_ok "${dest}/TakaroConnector.cs"
}

deploy_minecraft() {
    local platform="$1" dest jar subdir target tmp toolchain

    target="$(ds_target "minecraft-${platform}")"
    if [ -n "$target" ]; then
        # Catalog-driven: build the target's artifact and deploy it by manifest row, so
        # the jar in mods/ is the one the ledger records and nothing else is guessed at.
        toolchain=container
        if ds_have java && java -version 2>&1 | grep -qE '"(2[5-9]|[3-9][0-9])'; then
            toolchain=host
        fi
        tmp="$(mktemp -d)"
        trap 'rm -rf "$tmp"' RETURN
        ds_info "Building Minecraft target ${target} (${toolchain} toolchain)..."
        ds_maint build --game minecraft --target "$target" \
            --version "$("${REPO_ROOT}/scripts/dev-version.sh" minecraft)" \
            --out "$tmp" --toolchain "$toolchain"
        ds_maint deploy --game minecraft --target "$target" \
            --dest "$(ds_data_dir "minecraft-${platform}")" \
            --from "${tmp}/build-manifest.json"
        ds_ok "$(ds_data_dir "minecraft-${platform}")/mods"
        return 0
    fi

    # TODO(#151): paper and neoforge keep the old gradle+copy path until they
    # become catalog targets.
    ds_info "Building Minecraft ${platform} module (gradle)..."
    # Minecraft 26.2 (fabric) needs a JDK 25 toolchain; paper/neoforge still
    # target Java 21 class files but build fine on the same JDK 25.
    if ds_have java && java -version 2>&1 | grep -qE '"(2[5-9]|[3-9][0-9])'; then
        ( cd "${REPO_ROOT}/games/minecraft/mod" && ./gradlew ":${platform}:build" )
    else
        ds_info "No host JDK 25+ — building in eclipse-temurin:25-jdk"
        ds_toolchain_run eclipse-temurin:25-jdk "${REPO_ROOT}/games/minecraft/mod" \
            ./gradlew ":${platform}:build" --no-daemon
    fi

    jar="$(find "${REPO_ROOT}/games/minecraft/mod/${platform}/build/libs" \
        -name "takaro-${platform}-*.jar" \
        -not -name '*-dev-shadow*' -not -name '*-sources*' 2>/dev/null | head -1)"
    [ -n "$jar" ] || ds_die "no JAR built for ${platform}"

    # Paper loads plugins/, the mod loaders load mods/.
    [ "$platform" = "paper" ] && subdir="plugins" || subdir="mods"
    dest="$(ds_data_dir "minecraft-${platform}")/${subdir}"
    mkdir -p "$dest"
    cp "$jar" "${dest}/TakaroMinecraft.jar"
    ds_ok "${dest}/TakaroMinecraft.jar"
}

deploy_7d2d() {
    local dest
    ds_info "Preparing 7D2D build environment (SteamCMD + deps, first run is slow)..."
    ( cd "${REPO_ROOT}/games/7d2d" && ./scripts/setup-environment.sh )
    ds_info "Building 7D2D mod (Mono/MSBuild)..."
    ( cd "${REPO_ROOT}/games/7d2d" && ./scripts/build-mod.sh )

    dest="$(ds_data_dir 7d2d)/ServerFiles/Mods/Takaro"
    mkdir -p "$dest"
    cp -r "${REPO_ROOT}/games/7d2d/_data/build/Mods/Takaro/." "$dest/"
    ds_ok "$dest"
}

deploy_zomboid() {
    local dest jar
    # The agent module compiles against the game classes (compileOnly); stage
    # the jar from the bind mount / running container first.
    ds_info "Staging the Project Zomboid server jar (games/zomboid/scripts/setup-environment.sh)..."
    ( cd "${REPO_ROOT}/games/zomboid" && ./scripts/setup-environment.sh )

    # PZ B42 is class-file v69 (Java 25); a JDK 21 javac cannot read the game
    # jar, so the agent builds on a JDK 25 toolchain.
    ds_info "Building Zomboid connector (:agent:shadowJar)..."
    if ds_have java && java -version 2>&1 | grep -qE '"(2[5-9]|[3-9][0-9])'; then
        ( cd "${REPO_ROOT}/games/zomboid/mod" && ./gradlew :agent:shadowJar --no-daemon )
    else
        ds_info "No host JDK 25+ — building in eclipse-temurin:25-jdk"
        ds_toolchain_run eclipse-temurin:25-jdk "${REPO_ROOT}/games/zomboid/mod" \
            ./gradlew :agent:shadowJar --no-daemon
    fi

    jar="$(find "${REPO_ROOT}/games/zomboid/mod/agent/build/libs" \
        -name 'TakaroConnector-*.jar' -not -name '*-sources*' 2>/dev/null | head -1)"
    [ -n "$jar" ] || ds_die "no agent jar built for zomboid"

    # The agent lives in the persistent cache-dir mount (Zomboid/Takaro); the
    # install dir is reverted by SteamCMD validate on every start.
    dest="$(ds_data_dir zomboid)/config/Takaro"
    mkdir -p "$dest"
    cp "$jar" "${dest}/TakaroConnector.jar"
    ds_ok "${dest}/TakaroConnector.jar"
}

# Builds the Valheim toolchain image on first use, then reuses it.
ds_valheim_builder_image() {
    local tag="takaro-dev-valheim-builder"
    if ! docker image inspect "$tag" >/dev/null 2>&1; then
        ds_info "Building the Valheim toolchain image (one time)..." >&2
        docker build -q -t "$tag" "${DS_DIR}/images/valheim-builder" >&2
    fi
    printf '%s' "$tag"
}

deploy_valheim() {
    local stage dest version builder
    version="$(cat "${REPO_ROOT}/games/valheim/version.txt")"

    # The reference cache MUST stay separate from the runnable server:
    # games/valheim/scripts/setup-environment.sh refuses to write into a live install.
    stage="${REPO_ROOT}/games/valheim/_data/dist"
    mkdir -p "$stage"
    builder="$(ds_valheim_builder_image)"

    ds_info "Preparing Valheim compile references (SteamCMD + BepInEx)..."
    ds_toolchain_run "$builder" "${REPO_ROOT}/games/valheim" ./scripts/setup-environment.sh

    # Preferred path: the connector's own release script, which runs the full
    # test suite first and so also catches regressions.
    ds_info "Building Valheim connector v${version} via scripts/build-release.sh..."
    if ds_toolchain_run "$builder" "${REPO_ROOT}/games/valheim" \
           ./scripts/build-release.sh "$version" "$stage"; then
        rm -rf "${stage:?}/TakaroValheim"
        python3 -c '
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as zf:
    zf.extractall(sys.argv[2])
' "${stage}/takaro-valheim-plugin.zip" "$stage"
    else
        # Fall back to publishing the dedicated-server plugin directly. A
        # dedicated server needs only the DLLs, not the release archives, so an
        # unrelated packaging/docs test failure should not block a dev server.
        ds_warn "build-release.sh failed. Falling back to a direct plugin publish."
        ds_warn "This SKIPS the connector's test suite — fix the failing test before a real release."
        rm -rf "${stage:?}/TakaroValheim"
        ds_toolchain_run "$builder" "${REPO_ROOT}/games/valheim" \
            dotnet publish src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
                -c Release -f net472 -o "${stage}/TakaroValheim" \
                -p:EnableValheimPluginBuild=true \
                -p:BepInExReferencePath="${REPO_ROOT}/games/valheim/_data/deps/bepinex/BepInExPack_Valheim/BepInEx/core" \
                -p:ValheimReferencePath="${REPO_ROOT}/games/valheim/_data/server/valheim_server_Data/Managed" \
                -p:TakaroValheimReleaseVersion="$version" \
                -p:TakaroValheimBepInExVersion="$version" \
                -p:Version="$version" \
            || ds_die "Valheim plugin build failed"
    fi

    dest="$(ds_data_dir valheim)/config/bepinex/plugins"
    mkdir -p "$dest"
    rm -rf "${dest:?}/TakaroValheim"
    cp -r "${stage}/TakaroValheim" "${dest}/TakaroValheim"
    # A stale chainloader cache makes BepInEx skip the updated plugin.
    rm -f "$(ds_data_dir valheim)/server/bepinex/BepInEx/cache/chainloader_typeloader.dat"
    ds_ok "${dest}/TakaroValheim"
}

deploy_terraria() {
    local dest
    ds_info "Preparing TShock reference assemblies..."
    # games/terraria/scripts/build-mod.sh already falls back to a dotnet SDK container
    # when the host has no .NET 9 SDK, so no extra handling is needed here.
    ( cd "${REPO_ROOT}/games/terraria" && ./scripts/setup-environment.sh )
    ds_info "Building Terraria plugin..."
    ( cd "${REPO_ROOT}/games/terraria" && ./scripts/build-mod.sh )

    # The TShock image exposes /plugins as its -additionalplugins directory.
    dest="$(ds_data_dir terraria)/plugins"
    mkdir -p "$dest"
    cp "${REPO_ROOT}/games/terraria/_data/build/TakaroTerrariaEvents/TakaroTerrariaEvents.dll" "$dest/"
    ds_ok "${dest}/TakaroTerrariaEvents.dll"
}

deploy_conan() {
    # The sidecar runs straight from the connector directory, which compose
    # bind-mounts read-only. Building here is the whole deploy.
    ds_info "Building Conan Exiles sidecar (npm)..."
    if ds_have npm; then
        ( cd "${REPO_ROOT}/games/conan-exiles/bridge" && npm ci && npm run build )
    else
        ds_info "No host Node — building in node:22-slim"
        ds_toolchain_run node:22-slim "${REPO_ROOT}/games/conan-exiles/bridge" \
            sh -c "npm ci && npm run build"
    fi
    [ -f "${REPO_ROOT}/games/conan-exiles/bridge/dist/index.js" ] || ds_die "games/conan-exiles/bridge/dist/index.js missing after build"
    ds_ok "${REPO_ROOT}/games/conan-exiles/bridge/dist (mounted read-only into the bridge container)"
}

deploy_palworld() {
    # Third-party bridge; the image build downloads and checksums the pinned
    # release tarball. Nothing is built from this repo.
    ds_info "Building the third-party Palworld bridge image (pinned + checksummed)..."
    ds_compose palworld build palworld-bridge
    ds_ok "takaro-dev-palworld-bridge image"
}

DRAGONWILDS_SRC="${DRAGONWILDS_SRC:-${REPO_ROOT}/games/dragonwilds}"

deploy_dragonwilds() {
    local build="${DRAGONWILDS_SRC}/plugin/build.sh" dest="${DS_DATA}/dragonwilds-plugin"

    if [ ! -x "$build" ] && [ ! -f "$build" ]; then
        ds_warn "Dragonwilds plugin source not present yet (${build}) — nothing to deploy."
        ds_info "plugin source not present yet"
        return 0
    fi

    ds_info "Building libtakaro-dragonwilds.so..."
    ( cd "${DRAGONWILDS_SRC}/plugin" && bash ./build.sh ) || ds_die "Dragonwilds plugin build failed"
    local so="${DRAGONWILDS_SRC}/plugin/dist/libtakaro-dragonwilds.so"
    [ -f "$so" ] || ds_die "expected ${so} after build.sh"

    if [ -d "${DRAGONWILDS_SRC}/sidecar" ]; then
        ds_info "Rebuilding the Dragonwilds sidecar image..."
        ds_compose dragonwilds --profile sidecar build dragonwilds-takaro \
            || ds_die "Dragonwilds sidecar image build failed"
    else
        ds_warn "sidecar source not present yet (${DRAGONWILDS_SRC}/sidecar) — skipping its image build"
    fi

    # The running game holds the .so open via LD_PRELOAD, so it cannot be replaced in place:
    # stop → swap → start.
    local was_running=0
    if ds_is_running dragonwilds; then
        was_running=1
        ds_info "Stopping the rig to replace the plugin (the game process holds the .so open)..."
        ds_compose dragonwilds stop
    fi

    mkdir -p "$dest"
    cp "$so" "${dest}/libtakaro-dragonwilds.so.new"
    mv -f "${dest}/libtakaro-dragonwilds.so.new" "${dest}/libtakaro-dragonwilds.so"
    chmod 644 "${dest}/libtakaro-dragonwilds.so"
    ds_ok "${dest}/libtakaro-dragonwilds.so"

    if [ "$was_running" = "1" ]; then
        ds_info "Restarting the rig..."
        ds_compose dragonwilds start
    fi
}

ds_load_env

case "$GAME" in
    rust)               deploy_rust ;;
    minecraft-paper)    deploy_minecraft paper ;;
    minecraft-neoforge) deploy_minecraft neoforge ;;
    minecraft-fabric)   deploy_minecraft fabric ;;
    minecraft-fabric-26.1.2) deploy_minecraft fabric-26.1.2 ;;
    7d2d)               deploy_7d2d ;;
    zomboid)            deploy_zomboid ;;
    valheim)            deploy_valheim ;;
    terraria)           deploy_terraria ;;
    conan-exiles)       deploy_conan ;;
    palworld)           deploy_palworld ;;
    dragonwilds)        deploy_dragonwilds ;;
    *)                  ds_die "no deploy step defined for ${GAME}" ;;
esac

# Record what was just deployed so sync-connectors.sh can detect future drift.
ds_record_fingerprint "$GAME"
