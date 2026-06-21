# Valheim Takaro Connector

This source tree contains the Valheim Takaro connector.

The connector is a dedicated-server plugin. It is installed on the Valheim
dedicated server only; no player/client-side mod is required or supported.

## Shape

- `src/Takaro.Valheim.Core` contains game-independent Takaro protocol, config, player, and request handling code.
- `src/Takaro.Valheim.Plugin` contains the BepInEx/Jotunn plugin adapter for Valheim dedicated servers.
- `tests/Takaro.Valheim.Core.Tests` verifies the core behavior without requiring Valheim assemblies.

## Local Development

Run the core tests and reference-free plugin scaffold build:

```bash
dotnet test Takaro.Valheim.sln
```

The plugin project builds in reference-free scaffold mode by default. A real Valheim plugin build requires local BepInEx, Jotunn, and Valheim dedicated-server assemblies:

```bash
dotnet build src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -p:EnableValheimPluginBuild=true \
  -p:TargetFramework=net472 \
  -p:BepInExReferencePath=/path/to/BepInEx/core \
  -p:JotunnReferencePath=/path/to/BepInEx/plugins \
  -p:ValheimReferencePath=/path/to/valheim_server_Data/Managed
```

Smoke validation used BepInExPack Valheim `5.4.2333`, Jotunn `2.29.1`, and Valheim dedicated server `l-0.221.12`.

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

## Release Build

From inside `valheim/`:

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
- `giveItem`, `getPlayerInventory`, and `teleportPlayer` use only server-side
  Valheim state. If the dedicated server does not expose a live `Player`
  component for the target player, the connector returns a clear action error
  instead of relying on a client-side mod.
