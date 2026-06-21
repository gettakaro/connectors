# Takaro Terraria Events Plugin

Server-side TShock plugin for Terraria support in Takaro. The plugin emits
structured `TAKARO_EVENT` markers into the normal TShock log for events that
TShock REST does not expose directly, and it exposes small admin-only helper
commands for coordinate-based player location and teleport support.

This package is only the server-side Terraria mod/plugin. It does not include a
client mod or a sidecar process.

## Features

- Emits `TAKARO_EVENT` log markers for player deaths.
- Emits `TAKARO_EVENT` log markers for NPC kills.
- Registers `/takaropos <player>` to print a player's world coordinates.
- Registers `/takarotp <player> <x> <y>` to teleport a player to world
  coordinates.

## Build

Install the TShock reference DLLs once:

```bash
terraria/scripts/setup-environment.sh
```

Build the plugin:

```bash
terraria/scripts/build-mod.sh
```

The build output is written to:

```text
terraria/_data/build/TakaroTerrariaEvents/TakaroTerrariaEvents.dll
```

## Package

```bash
terraria/scripts/build-release.sh 0.1.0 dist
```

This creates:

```text
dist/takaro-terraria-plugin.zip
```

## Install

1. Install TShock on the Terraria dedicated server.
2. Copy `TakaroTerrariaEvents.dll` into the TShock server plugin directory.
3. Restart the server.
4. Confirm the TShock log contains `Takaro Terraria Events plugin loaded`.

The plugin writes event markers to the normal TShock log. A Takaro connector or
bridge can tail those logs and map `TAKARO_EVENT` records into Takaro events.

## Runtime Commands

```text
/takaropos <player>
/takarotp <player> <x> <y>
```

Both commands are server-side TShock commands. They are intended for connector
automation and require the `takaro.admin` TShock permission.
