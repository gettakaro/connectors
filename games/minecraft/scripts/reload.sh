#!/usr/bin/env bash
set -euo pipefail

PLATFORM="${1:-all}"
RCON_PASS="${RCON_PASSWORD:-takaro123}"

reload_paper() {
    echo "Reloading Paper server..."
    docker exec minecraft-paper rcon-cli --password "$RCON_PASS" "reload confirm" 2>/dev/null || \
        echo "  Warning: Could not connect to Paper RCON"
}

reload_neoforge() {
    echo "NeoForge does not support hot reload. Restart the container:"
    echo "  docker compose restart neoforge"
}

reload_fabric() {
    echo "Fabric does not support hot reload. Restart the container:"
    echo "  docker compose restart fabric"
}

case "$PLATFORM" in
    paper)    reload_paper ;;
    neoforge) reload_neoforge ;;
    fabric)   reload_fabric ;;
    all)
        reload_paper
        reload_neoforge
        reload_fabric
        ;;
    *)
        echo "Usage: $0 {paper|neoforge|fabric|all}"
        exit 1
        ;;
esac
