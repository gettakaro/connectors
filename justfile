# Takaro Game Server Connectors

# === Release ===

# Releases are automated by release-please from Conventional Commits. A per-connector
# "Release PR" is maintained on `main`; merging it bumps the version, tags
# <connector>-v*, updates the CHANGELOG, and publishes a GitHub Release with artifacts.
# To force a specific version, add a `Release-As: X.Y.Z` footer to a commit on main.

# Print the rolling dev-build version for a connector (rust, minecraft, 7d2d, valheim, conan-exiles, terraria)
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

# Build the Project Zomboid connector release artifact locally into <out-dir>
build-release-zomboid version out-dir='dist':
    ./zomboid/scripts/build-release.sh {{version}} {{out-dir}}

# Build the Conan Exiles connector release artifact locally into <out-dir>
build-release-conan version out-dir='dist':
    ./conan-exiles/scripts/build-release.sh {{version}} {{out-dir}}

# Build the Terraria TShock plugin release artifact locally into <out-dir>
build-release-terraria version out-dir='dist':
    ./terraria/scripts/build-release.sh {{version}} {{out-dir}}

# Build the Terraria bridge release artifact locally into <out-dir>
build-release-terraria-bridge version out-dir='dist':
    ./terraria/scripts/build-bridge-release.sh {{version}} {{out-dir}}

# Build the Valheim connector release artifacts locally into <out-dir>
build-release-valheim version out-dir='dist':
    ./valheim/scripts/build-release.sh {{version}} {{out-dir}}

# === Terraria Plugin ===

# Prepare TShock reference assemblies
terraria-setup:
    cd games/terraria && ./scripts/setup-environment.sh

# Build the Terraria TShock plugin
terraria-build:
    cd games/terraria && ./scripts/build-mod.sh

# Install Terraria bridge dependencies
terraria-bridge-install:
    cd games/terraria/bridge && npm ci

# Run Terraria bridge tests
terraria-bridge-test:
    cd games/terraria/bridge && npm test

# Build the Terraria bridge
terraria-bridge-build:
    cd games/terraria/bridge && npm run build

# === Rust Connector ===

# Start the Rust dev server
rust-up *args:
    cd games/rust && docker compose up {{args}}

# Stop the Rust dev server
rust-down *args:
    cd games/rust && docker compose down {{args}}

# View Rust server logs
rust-logs *args='--tail 100 -f':
    cd games/rust && docker compose logs {{args}}

# Deploy the Rust plugin to the dev server
rust-deploy:
    cd games/rust && ./scripts/deploy.sh

# Hot-reload the Rust plugin via RCON
rust-reload:
    cd games/rust && ./scripts/reload.sh

# === Minecraft Connector ===

# Build all Minecraft connector modules
minecraft-build *args:
    cd games/minecraft/mod && ./gradlew build {{args}}

# Build a single Minecraft module (paper, fabric, neoforge, core)
minecraft-build-module module *args:
    cd games/minecraft/mod && ./gradlew :{{module}}:build {{args}}

# Start Minecraft dev server(s)
minecraft-up *args:
    cd games/minecraft && docker compose up {{args}}

# Stop Minecraft dev server(s)
minecraft-down *args:
    cd games/minecraft && docker compose down {{args}}

# View Minecraft server logs
minecraft-logs *args='--tail 100 -f':
    cd games/minecraft && docker compose logs {{args}}

# Deploy Minecraft JARs to dev server(s)
minecraft-deploy platform:
    cd games/minecraft && ./scripts/deploy.sh {{platform}}

# Reload Minecraft plugin via RCON
minecraft-reload platform:
    cd games/minecraft && ./scripts/reload.sh {{platform}}

# Start the Minecraft test bot
minecraft-bot-up *args:
    cd games/minecraft && docker compose up bot {{args}}

# === 7D2D Connector ===

# Prepare 7D2D build dependencies and game binaries
sevend2d-setup:
    cd games/7d2d && ./scripts/setup-environment.sh

# Build the 7D2D mod
sevend2d-build:
    cd games/7d2d && ./scripts/build-mod.sh

# Build and deploy the 7D2D mod to the local test server
sevend2d-build-deploy:
    cd games/7d2d && ./scripts/build-mod.sh deploy

# Run the Generic Connector protocol contract harness
sevend2d-test-contract:
    cd games/7d2d && ./scripts/test-contract.sh

# Run the 7D2D source-level regression suite
sevend2d-test-regressions:
    python3 -m unittest tests/test_7d2d_connector_regressions.py

# Start the 7D2D dev services
sevend2d-up *args:
    cd games/7d2d && docker compose up {{args}}

# Stop the 7D2D dev services
sevend2d-down *args:
    cd games/7d2d && docker compose down {{args}}

# View 7D2D service logs
sevend2d-logs *args='--tail 100 -f':
    cd games/7d2d && docker compose logs {{args}}

# === Project Zomboid Connector ===

# Stage the PZ server jar so the agent can compile against the game classes
zomboid-setup:
    cd games/zomboid && ./scripts/setup-environment.sh

# Build the Zomboid connector (unit tests + shaded -javaagent jar; needs JDK 25)
zomboid-build *args:
    cd games/zomboid/mod && ./gradlew build {{args}}

# Build the Zomboid agent from the working tree and deploy it into dev-servers/_data
zomboid-deploy:
    ./dev-servers/scripts/deploy-connector.sh zomboid

# Start the Zomboid dev server
zomboid-up *args='-d':
    docker compose -f dev-servers/compose/zomboid.yml --env-file dev-servers/.env up {{args}}

# Stop the Zomboid dev server
zomboid-down *args:
    docker compose -f dev-servers/compose/zomboid.yml --env-file dev-servers/.env down {{args}}

# View Zomboid server logs
zomboid-logs *args='--tail 100 -f':
    docker compose -f dev-servers/compose/zomboid.yml --env-file dev-servers/.env logs {{args}}

# === Conan Exiles Connector ===

# Install Conan Exiles connector dependencies
conan-install:
    cd games/conan-exiles/bridge && npm ci

# Run Conan Exiles connector tests
conan-test:
    cd games/conan-exiles/bridge && npm test

# Build the Conan Exiles connector
conan-build:
    cd games/conan-exiles/bridge && npm run build

# === Dev Servers (dev-servers/) ===

# Unified test environment: every game with its real Takaro connector installed.
# See dev-servers/README.md. One-time: cp dev-servers/.env.example dev-servers/.env

# Install every game (or the ones named), strictly one at a time
dev-install-all *args:
    ./dev-servers/scripts/install-all.sh {{args}}

# Install a single game
dev-install game *args:
    ./dev-servers/scripts/install.sh {{game}} {{args}}

# Rebuild a connector from the working tree and deploy it into dev-servers/_data
dev-deploy game:
    ./dev-servers/scripts/deploy-connector.sh {{game}}

# Start one or more games (refuses to exceed the RAM budget)
dev-start *args:
    ./dev-servers/scripts/start.sh {{args}}

# Stop games; with no arguments, stops everything
dev-stop *args:
    ./dev-servers/scripts/stop.sh {{args}}

# Re-render Takaro configs after setting the token (no reinstall, no re-download)
dev-reconfigure *args:
    ./dev-servers/scripts/reconfigure.sh {{args}}

# Show what is installed, running, and what it costs
dev-status:
    ./dev-servers/scripts/status.sh

# Tail a game's logs
dev-logs game *args:
    ./dev-servers/scripts/logs.sh {{game}} {{args}}

# Report which deployed connectors have gone stale vs the working tree
dev-sync-check:
    ./dev-servers/scripts/sync-connectors.sh --check

# Rebuild and redeploy any connector whose source changed (--restart to restart them)
dev-sync *args:
    ./dev-servers/scripts/sync-connectors.sh {{args}}

# Install git hooks that check connector staleness after pull/merge/checkout
dev-install-hooks *args:
    ./dev-servers/scripts/install-git-hooks.sh {{args}}

# Validate every dev-servers compose file and check for port collisions
dev-validate:
    ./dev-servers/scripts/validate.sh
