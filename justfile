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

# Create a full release for the 7D2D connector
release-7d2d version:
    git tag "7d2d-v{{version}}"
    git push origin "7d2d-v{{version}}"

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

# === 7D2D Connector ===

# Prepare 7D2D build dependencies and game binaries
sevend2d-setup:
    cd 7d2d && ./scripts/setup-environment.sh

# Build the 7D2D mod
sevend2d-build:
    cd 7d2d && ./scripts/build-mod.sh

# Build and deploy the 7D2D mod to the local test server
sevend2d-build-deploy:
    cd 7d2d && ./scripts/build-mod.sh deploy

# Start the 7D2D dev services
sevend2d-up *args:
    cd 7d2d && docker compose up {{args}}

# Stop the 7D2D dev services
sevend2d-down *args:
    cd 7d2d && docker compose down {{args}}

# View 7D2D service logs
sevend2d-logs *args='--tail 100 -f':
    cd 7d2d && docker compose logs {{args}}
