# Takaro Game Server Connectors

# === Release ===

# Create a full release for the Rust connector
release-rust version:
    git tag "rust-v{{version}}"
    git push origin "rust-v{{version}}"

# Create a full release for the Minecraft connector
release-minecraft version:
    git tag "minecraft-v{{version}}"
    git push origin "minecraft-v{{version}}"

# === Rust Connector ===

# Start the Rust dev server
rust-up *args:
    cd rust && docker compose up {{args}}

# Stop the Rust dev server
rust-down *args:
    cd rust && docker compose down {{args}}

# View Rust server logs
rust-logs *args='--tail 100 -f':
    cd rust && docker compose logs {{args}}

# Deploy the Rust plugin to the dev server
rust-deploy:
    cd rust && ./scripts/deploy.sh

# Hot-reload the Rust plugin via RCON
rust-reload:
    cd rust && ./scripts/reload.sh

# === Minecraft Connector ===

# Build all Minecraft connector modules
minecraft-build *args:
    cd minecraft && ./gradlew build {{args}}

# Build a single Minecraft module (paper, fabric, neoforge, core)
minecraft-build-module module *args:
    cd minecraft && ./gradlew :{{module}}:build {{args}}

# Start Minecraft dev server(s)
minecraft-up *args:
    cd minecraft && docker compose up {{args}}

# Stop Minecraft dev server(s)
minecraft-down *args:
    cd minecraft && docker compose down {{args}}

# View Minecraft server logs
minecraft-logs *args='--tail 100 -f':
    cd minecraft && docker compose logs {{args}}

# Deploy Minecraft JARs to dev server(s)
minecraft-deploy platform:
    cd minecraft && ./scripts/deploy.sh {{platform}}

# Reload Minecraft plugin via RCON
minecraft-reload platform:
    cd minecraft && ./scripts/reload.sh {{platform}}

# Start the Minecraft test bot
minecraft-bot-up *args:
    cd minecraft && docker compose up bot {{args}}
