# Takaro RuneScape: Dragonwilds Connector

A native library, `libtakaro-dragonwilds.so`, that the RuneScape: Dragonwilds Linux dedicated
server loads at start. It connects to Takaro by itself; there is no sidecar. Players do not install
anything.

## Install

### 1. Prepare the server

- A Dragonwilds **Linux** dedicated server (Steam app 4019830) whose launch command you can change.
  Keep `RSDragonwildsServer-Linux-Shipping.sym` next to the server binary (`app_update 4019830 validate` restores it).
- Set the server's `OwnerId` to your EOS Player ID (bottom of the game client's Settings);
  without it the server never loads a world.
- Client and server must run the same game version.
- In Takaro, create a game server of type **Generic** and copy its registration token.

Find your server build: the `"buildid"` value in `steamapps/appmanifest_4019830.acf`. Supported
builds: 26161880 (current), 26087591, 25991736, 25808123, 25630937, 25501739 and 25465077.

### 2. Download

From <https://takaro.io/connectors/dragonwilds> or the latest `dragonwilds-v*` release on
<https://github.com/gettakaro/connectors/releases>, download
`takaro-dragonwilds-plugin-linux-<build>-<version>.tar.gz` for your build. Unpack it and copy
`TakaroDragonwilds/libtakaro-dragonwilds.so` to a folder **outside** the Steam install (SteamCMD
`validate` deletes files it does not know), for example `/opt/takaro/`.

### 3. Load the library

Preload it on the game binary only. SteamCMD is 32-bit and fails with the preload, so never set
`LD_PRELOAD` for the whole container, user or service:

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so ./RSDragonwildsServer.sh -log
```

With Docker, mount the library folder writable at `/opt/takaro` and a persistent folder at
`/opt/takaro-state` (set `TAKARO_STATE_DIR=/opt/takaro-state`). The archive's
`docker-compose.example.yml` shows the setup.

### 4. Set the token

On first start the connector creates `takaro.cfg` next to the library. Paste the token after
`TAKARO_REGISTRATION_TOKEN=` and save. It connects within a few seconds, no restart needed.

| Key | What to put there |
|---|---|
| `TAKARO_REGISTRATION_TOKEN` | The Generic game server's registration token. |
| `TAKARO_IDENTITY_TOKEN` | Filled in by the connector; keep it. |
| `TAKARO_SERVER_NAME` | Name shown in Takaro; must be unique. Empty uses `Dragonwilds (<id>)`. |
| `TAKARO_PROTOCOL` | Optional: `0` limits the connector to Takaro protocol 0. Needs a restart. |

The same keys can be set as environment variables; those win over `takaro.cfg` and need a restart.

### 5. Check that it worked

The server console shows `[Takaro] connected to Takaro as "..."`, and Takaro shows the server online
within a minute of the world loading. If something is wrong, a `[Takaro]` banner names the file to
fix. The connector's log is `RSDragonwilds/Binaries/Linux/takaro/plugin.log` (Docker: in `TAKARO_STATE_DIR`).

### Update

Stop the server, replace `libtakaro-dragonwilds.so` with the one for your build, start it again.
`takaro.cfg` is kept. Coming from the 0.2.x sidecar: follow [INSTALL.md](INSTALL.md) instead, the
old sidecar must be stopped first.

## What works, what doesn't

Tested on server build 26161880 (game 1.0.1.3) on 2026-10-26.

| Feature | Status | Note |
|---|---|---|
| Connection and reconnect | ✅ | Survives server restarts and network drops. |
| Player list and lookup | ✅ | Character name, EOS and Steam IDs. |
| Player location and inventory | ✅ | |
| Give item | ✅ | No relog needed. |
| Item catalogue | ✅ | In-game names. |
| Entity catalogue | ⚠️ | Only creatures the server has loaded so far. |
| Locations | ⚠️ | Lodestones only. |
| Console commands | ✅ | Connector command set; unknown commands are refused. |
| Broadcast and whisper | ✅ | Shown with a `[Takaro]` prefix. |
| Teleport | ✅ | |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent; banned players cannot log in. |
| Ban list | ⚠️ | Shows the EOS ID, not the Steam ID. |
| Shutdown | ✅ | Saves first, then stops. |
| Join, leave and chat events | ✅ | |
| Death events | ✅ | Creature, fall and environment causes, with position. |
| Entity kill events | ✅ | Creature name and held weapon. |
| Log events | ✅ | World password removed. |
| Map | ⚠️ | Needs Takaro protocol 1 and an uploaded map image. |
| Game name, version and icon | ✅ | Needs Takaro protocol 1. |
| Modules (commands, hooks, cronjobs, teleports) | ✅ | |
| Shop and economy | ✅ | Bundles and offline orders included. |
| Discord chat both ways | ✅ | Use the `chatBridgeNoEcho` module. |
| Events during a Takaro outage | ✅ | Queued on disk and delivered after reconnect. |
| Whisper hidden from other players | ⚠️ | Not tested with two players. |

## Known issues

- The server idles until `OwnerId` is set to the owner's EOS Player ID.
- The server has no map picture; [DEVELOPMENT.md](DEVELOPMENT.md) shows how to make one to upload.
