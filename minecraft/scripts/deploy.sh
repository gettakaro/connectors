#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PLATFORM="${1:-all}"

find_jar() {
    local module="$1"
    local jar
    # Find the main JAR (not dev-shadow, not sources)
    jar=$(find "$module/build/libs" -name "takaro-${module}-*.jar" \
        -not -name "*-dev-shadow*" -not -name "*-sources*" 2>/dev/null | head -1)
    if [ -z "$jar" ]; then
        echo "Error: No JAR found for $module. Run ./gradlew build first." >&2
        exit 1
    fi
    echo "$jar"
}

deploy_paper() {
    local jar
    jar=$(find_jar paper)
    echo "Deploying Paper plugin..."
    mkdir -p _data/paper/plugins
    cp "$jar" _data/paper/plugins/TakaroMinecraft.jar
    echo "  -> _data/paper/plugins/TakaroMinecraft.jar"
}

deploy_neoforge() {
    local jar
    jar=$(find_jar neoforge)
    echo "Deploying NeoForge mod..."
    mkdir -p _data/neoforge/mods
    cp "$jar" _data/neoforge/mods/TakaroMinecraft.jar
    echo "  -> _data/neoforge/mods/TakaroMinecraft.jar"
}

deploy_fabric() {
    local jar
    jar=$(find_jar fabric)
    echo "Deploying Fabric mod..."
    mkdir -p _data/fabric/mods
    cp "$jar" _data/fabric/mods/TakaroMinecraft.jar
    echo "  -> _data/fabric/mods/TakaroMinecraft.jar"
}

case "$PLATFORM" in
    paper)    deploy_paper ;;
    neoforge) deploy_neoforge ;;
    fabric)   deploy_fabric ;;
    all)
        deploy_paper
        deploy_neoforge
        deploy_fabric
        ;;
    *)
        echo "Usage: $0 {paper|neoforge|fabric|all}"
        exit 1
        ;;
esac

echo "Done."
