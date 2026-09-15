# Rust Connector — Development

Developer notes for the Takaro Rust connector. Operators want [README.md](README.md) instead.

## Architecture

The plugin implements the [Takaro Generic Connector Protocol](https://docs.takaro.io/advanced/adding-support-for-a-new-game).
It runs inside the Rust server via the [Carbon](https://carbonmod.gg/) mod framework (it is written
against the Oxide/uMod plugin API, so Oxide loads it too) and connects outbound to Takaro over a
WebSocket. No port forwarding required.

```
[Takaro Backend]  <->  [TakaroConnector.cs]  <->  [Rust Server + Carbon]
                 WebSocket (outbound)         Direct C# game API access
```

Everything lives in one file: `mod/TakaroConnector.cs`.

## Features

- All 17 Takaro actions: `testReachability`, `getPlayer`, `getPlayers`, `getPlayerLocation`,
  `getPlayerInventory`, `listItems`, `listEntities`, `listLocations`, `executeConsoleCommand`,
  `sendMessage`, `giveItem`, `teleportPlayer`, `kickPlayer`, `banPlayer`, `unbanPlayer`,
  `listBans`, `shutdown`.
- 6 game events: `player-connected`, `player-disconnected`, `chat-message`, `player-death`,
  `entity-killed`, `log`.
- WebSocket with automatic reconnection and exponential backoff (5 s initial, 1.5x, 5 min cap).
- Hot-reload support via Carbon (no server restart needed).

## Dev environment

### Prerequisites

- Docker
- Node.js v22+ (for the RCON reload script)
- A Takaro account with a registration token ([get one from your Takaro dashboard](https://docs.takaro.io/advanced/adding-support-for-a-new-game))

### Quick start

```bash
# Configure (from repo root)
cp .env.example .env
# Edit .env: set TAKARO_WS_URL and TAKARO_REGISTRATION_TOKEN

# Build and start the Rust server (first build downloads ~6GB)
cd games/rust
docker compose up -d rust

# Wait for the server to finish booting (first boot takes several minutes)
# Check with: docker compose logs -f rust | grep "Server startup complete"

# Deploy plugin
./scripts/deploy.sh

# Hot-reload
./scripts/reload.sh
```

`docker-compose.yml` mounts `_data/plugins` at `/rust/carbon/plugins`, so `deploy.sh` just copies
`mod/TakaroConnector.cs` into `_data/plugins/`.

### Environment variables

| Variable | Description | Default |
|----------|-------------|---------|
| `TAKARO_WS_URL` | Takaro WebSocket endpoint | `wss://connect.takaro.io/` |
| `TAKARO_REGISTRATION_TOKEN` | Server registration token | (required) |
| `TAKARO_IDENTITY_TOKEN` | Unique server identity | `takaro-rust-dev` |
| `TAKARO_DEBUG` | Enable debug logging | `false` |
| `RCON_PASSWORD` | RCON password (dev only) | `takaro123` |

### Edit loop

```bash
# Edit the plugin
vim mod/TakaroConnector.cs

# Deploy and reload (no build step — Carbon compiles .cs at runtime)
./scripts/deploy.sh
./scripts/reload.sh  # requires Node.js v22+

# View logs
just rust-logs
```

## Build and release

- `scripts/build-release.sh <version> <out-dir>` stamps `<version>` into the `[Info(...)]`
  attribute and copies `TakaroConnector.cs` into `<out-dir>`. That single `.cs` file is the entire
  release artifact.
- `.github/workflows/rust.yml`:
  - **compile-check** — pulls the Rust dedicated server managed DLLs via SteamCMD (cached) plus the
    Carbon release, generates a `.csproj` referencing both, and builds the plugin.
  - **docker-build** — builds `games/rust/Dockerfile` on pull requests.
  - **package** — runs `build-release.sh` and publishes `TakaroConnector.cs`: to the `rust-v*` tag
    on a release run, to the rolling `rust-dev` pre-release on pushes to `main`, and to a
    disposable `pr-<n>-rust` pre-release (with a PR comment) on same-repo pull requests.
- Versioning is release-please driven; the `// x-release-please-version` marker on the `[Info(...)]`
  line in `mod/TakaroConnector.cs` and `version.txt` are the version sources.

## Testing status

There is no recorded hard-test evidence for the Rust connector — no live run against a real Rust
server with a real client has been captured. Before any row in the README's status table becomes
✅, a live test needs to be run and its evidence recorded.
