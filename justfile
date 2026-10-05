# Takaro Game Server Connectors

# === Release ===

# Releases are automated by release-please from Conventional Commits. A per-connector
# "Release PR" is maintained on `main`; merging it bumps the version, tags
# <connector>-v*, updates the CHANGELOG, and publishes a GitHub Release with artifacts.
# To force a specific version, add a `Release-As: X.Y.Z` footer to a commit on main.

# Print the rolling dev-build version for a connector (rust, minecraft, 7d2d, valheim, conan-exiles, terraria)
dev-version connector:
    ./scripts/dev-version.sh {{connector}}

# === Maintenance ===

# Run the catalog-driven maintenance command (targets list/resolve, install, build, deploy, verify)
maint *args:
    ./maintenance/bin/takaro-maint {{args}}

# Build the pinned maintenance tool container (uv + managed Python 3.12 + steamcmd + DepotDownloader)
maint-container-build tag='takaro-maint:local':
    docker build --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" -t {{tag}} -f maintenance/Dockerfile maintenance

# Run takaro-maint inside the tool container against this checkout; credentials pass through from the environment by name only
maint-container *args:
    docker run --rm -v "$PWD:/repo" -e GH_TOKEN -e TAKARO_MAINT_REPO -e TAKARO_MAINT_GITHUB_API_URL $(env | grep -oE '^TAKARO_MAINT_STEAM_BRANCH_PASSWORD__[A-Za-z0-9_]+' | sed 's/^/-e /') takaro-maint:local {{args}}

# Build the Rust connector release artifact locally into <out-dir>
build-release-rust version out-dir='dist':
    ./games/rust/scripts/build-release.sh {{version}} {{out-dir}}

# Build the Minecraft connector release artifacts locally into <out-dir>
build-release-minecraft version out-dir='dist':
    ./games/minecraft/scripts/build-release.sh {{version}} {{out-dir}}

# Build the 7D2D connector release artifact locally into <out-dir>
build-release-7d2d version out-dir='dist' target='linux-3.2.0.b10':
    ./games/7d2d/scripts/build-release.sh {{version}} {{out-dir}} --target {{target}}

# Build the Project Zomboid connector release artifact locally into <out-dir>
build-release-zomboid version out-dir='dist':
    ./games/zomboid/scripts/build-release.sh {{version}} {{out-dir}}

# Build the Conan Exiles connector release artifact locally into <out-dir>
build-release-conan version out-dir='dist':
    ./games/conan-exiles/scripts/build-release.sh {{version}} {{out-dir}}

# Build the Terraria TShock plugin release artifact locally into <out-dir>
build-release-terraria version out-dir='dist':
    ./games/terraria/scripts/build-release.sh {{version}} {{out-dir}}

# Build the Terraria bridge release artifact locally into <out-dir>
build-release-terraria-bridge version out-dir='dist':
    ./games/terraria/scripts/build-bridge-release.sh {{version}} {{out-dir}}

# Build the Valheim connector release artifacts locally into <out-dir>
build-release-valheim version out-dir='dist':
    ./games/valheim/scripts/build-release.sh {{version}} {{out-dir}}

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

# Deploy the Rust plugin to the dev-servers rig
rust-deploy:
    ./dev-servers/scripts/deploy-connector.sh rust

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

# Deploy Minecraft JARs to the dev-servers rig (platform: paper|neoforge|fabric)
minecraft-deploy platform:
    ./dev-servers/scripts/deploy-connector.sh minecraft-{{platform}}

# Reload Minecraft plugin via RCON
minecraft-reload platform:
    cd games/minecraft && ./scripts/reload.sh {{platform}}

# Start the Minecraft test bot (the only service left in games/minecraft/docker-compose.yml)
minecraft-bot-up *args:
    cd games/minecraft && docker compose up bot {{args}}

# === 7D2D Connector ===

# Prepare 7D2D build dependencies and game binaries
sevend2d-setup target='linux-3.2.0.b10':
    cd games/7d2d && ./scripts/setup-environment.sh --target {{target}}

# Build the 7D2D mod
sevend2d-build target='linux-3.2.0.b10':
    cd games/7d2d && ./scripts/build-mod.sh --target {{target}}

# Build the 7D2D mod and deploy it into the dev-servers rig
sevend2d-build-deploy:
    ./dev-servers/scripts/deploy-connector.sh 7d2d

# Run the Generic Connector protocol contract harness
sevend2d-test-contract target='linux-3.2.0.b10':
    cd games/7d2d && ./scripts/test-contract.sh --target {{target}}

# Run the 7D2D source-level regression suite
sevend2d-test-regressions:
    python3 -m unittest tests/test_7d2d_connector_regressions.py

# Read what Steam serves on the pinned branch now and compare it with the catalog
sevend2d-pin *args:
    ./maintenance/bin/takaro-maint steam pin --game 7d2d {{args}}

# Start the 7D2D build services (builder/deps; the test server lives in dev-servers/)
sevend2d-up *args:
    ./games/7d2d/scripts/compose.sh up {{args}}

# Stop the 7D2D build services
sevend2d-down *args:
    ./games/7d2d/scripts/compose.sh down {{args}}

# View 7D2D build service logs
sevend2d-logs *args='--tail 100 -f':
    ./games/7d2d/scripts/compose.sh logs {{args}}

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

# Declare which games this box is working on and reconcile the running set
dev-focus *args:
    ./dev-servers/scripts/focus.sh {{args}}

# Validate every dev-servers compose file and check for port collisions
dev-validate:
    ./dev-servers/scripts/validate.sh
