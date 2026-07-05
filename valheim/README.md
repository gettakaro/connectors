# Valheim Takaro Connector

This source tree contains the Valheim Takaro connector.

## Shape

- `src/Takaro.Valheim.Core` contains game-independent Takaro protocol, config, player, and request handling code.
- `src/Takaro.Valheim.Plugin` contains the BepInEx plugin adapter for Valheim dedicated servers.
- `tests/Takaro.Valheim.Core.Tests` verifies the core behavior without requiring Valheim assemblies.

## Local Development

Run the core tests and reference-free plugin scaffold build:

```bash
dotnet test Takaro.Valheim.sln
```

The plugin project builds in reference-free scaffold mode by default. A real Valheim plugin build requires local BepInEx and Valheim dedicated-server assemblies:

```bash
dotnet build src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -p:EnableValheimPluginBuild=true \
  -p:TargetFramework=net472 \
  -p:BepInExReferencePath=/path/to/BepInEx/core \
  -p:ValheimReferencePath=/path/to/valheim_server_Data/Managed
```

Smoke validation used BepInExPack Valheim `5.4.2333` and Valheim dedicated server `l-0.221.12`. Jotunn is not required.

Production Takaro registration smoke validation connected to `wss://connect.takaro.io/`, sent `identify`, and received identification confirmation. Do not commit live registration or identity tokens to this repository.

## Runtime Config

The plugin reads BepInEx config values equivalent to:

- `registrationToken`
- `serverName`
- `identityToken`
- `takaroWsUrl`
- `logLevel`
- `enableLogEvents`

The default WebSocket URL is `wss://connect.takaro.io/`.

## Optional Client Command Bridge

The connector is server-side by default. If you want Takaro chat commands typed in the Valheim client to reach Takaro, install the same `TakaroValheim` BepInEx plugin folder in the Valheim client.

The client-side path is intentionally narrow:

- It only forwards chat text matching configured command prefixes, default `$`.
- It forwards through Valheim `ZRoutedRpc` to the dedicated server plugin as `TakaroClientChatCommand`.
- It does not connect directly to Takaro.
- It does not forward inventory, location, death, entity-kill, or general non-command chat state.

The command prefixes can be configured in the client BepInEx config value `Takaro.clientCommandPrefixes` as a semicolon-separated list.

## Release Build

From the monorepo root:

```bash
just valheim-setup
just build-release-valheim 0.1.0
```

Or from inside `valheim/`:

```bash
./scripts/setup-environment.sh
./scripts/build-release.sh 0.1.0 dist
```

The release artifact is `takaro-valheim-plugin.zip`.

## Known Caveats

- `listLocations` is implemented and live-smoked, but dashboard consumers should
  verify the final location DTO shape against Takaro's expected nested
  `position` plus `radius`/`sizeX` format.
- Destructive admin actions such as bans, kicks, and shutdown should be tested
  on a disposable server before production use.
