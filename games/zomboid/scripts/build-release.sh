#!/usr/bin/env bash
# Builds the Project Zomboid connector agent at <version> and collects the
# shaded -javaagent jar into <out-dir>. Runs locally and in CI.
#
# The agent links against the game's classes (compileOnly projectzomboid.jar),
# which are class-file v69 (Java 25); run this on a JDK 25 toolchain. The jar is
# staged by scripts/setup-environment.sh (local: dev-servers bind mount /
# running container; CI: SteamCMD app 380870).
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: build-release.sh <version> <out-dir>}"
OUT_DIR="${2:?usage: build-release.sh <version> <out-dir>}"

mkdir -p "$OUT_DIR"
echo "Building Project Zomboid connector v${VERSION}..."

# Stage projectzomboid.jar so the agent module can compile against the game.
./scripts/setup-environment.sh

(cd mod && ./gradlew :agent:shadowJar -Pversion="${VERSION}" --no-daemon)

jar=$(find "mod/agent/build/libs" -name "TakaroConnector-*.jar" \
    -not -name "*-sources*" 2>/dev/null | head -1)
[ -n "$jar" ] || { echo "Error: no agent jar found" >&2; exit 1; }

cp "$jar" "$OUT_DIR/"
echo "  -> $OUT_DIR/$(basename "$jar")"
