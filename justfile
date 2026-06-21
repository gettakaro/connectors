# Takaro Game Server Connectors

# === Release ===

# Releases are automated by release-please from Conventional Commits. A per-connector
# "Release PR" is maintained on `main`; merging it bumps the version, tags
# <connector>-v*, updates the CHANGELOG, and publishes a GitHub Release with artifacts.
# To force a specific version, add a `Release-As: X.Y.Z` footer to a commit on main.

# Print the rolling dev-build version for a connector (rust, minecraft, 7d2d, valheim, conan-exiles)
dev-version connector:
    ./scripts/dev-version.sh {{connector}}

# Build the Rust connector release artifact locally into <out-dir>
build-release-rust version out-dir='dist':
    ./rust/scripts/build-release.sh {{version}} {{out-dir}}

# Build the Minecraft connector release artifacts locally into <out-dir>
build-release-minecraft version out-dir='dist':
    ./minecraft/scripts/build-release.sh {{version}} {{out-dir}}

# Build the 7D2D connector release artifact locally into <out-dir>
build-release-7d2d version out-dir='dist':
    ./7d2d/scripts/build-release.sh {{version}} {{out-dir}}

# Build the Conan Exiles connector release artifact locally into <out-dir>
build-release-conan version out-dir='dist':
    ./conan-exiles/scripts/build-release.sh {{version}} {{out-dir}}

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

# === Conan Exiles Connector ===

# Install Conan Exiles connector dependencies
conan-install:
    cd conan-exiles && npm ci

# Run Conan Exiles connector tests
conan-test:
    cd conan-exiles && npm test

# Build the Conan Exiles connector
conan-build:
    cd conan-exiles && npm run build
