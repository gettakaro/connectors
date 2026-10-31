# Takaro ARK: Survival Evolved Connector

A native plugin that runs inside the **Linux** ARK: Survival Evolved dedicated server. Players do
not install anything.

## Install

You need: an ARK: Survival Evolved **Linux** dedicated server on build **21241282** (Windows and
other builds are not supported), control over its start command, and a Takaro game server of type
**Generic** with its registration token.

### 1. Download

From https://takaro.io/connectors/ark, download `takaro-ark-native-linux-21241282-<version>.zip`.

### 2. Install

Stop the server and unzip into a `TakaroArk/` folder in your ARK install folder (the one that
contains `ShooterGame/`):

```sh
cd /path/to/ark && mkdir -p TakaroArk && unzip takaro-ark-native-linux-21241282-<version>.zip -d TakaroArk
```

From now on, start the server through the launcher instead of `ShooterGameServer`:

```sh
/path/to/ark/TakaroArk/TakaroArkNative/launch.sh /path/to/ark 'TheIsland?listen?SessionName=My-ARK?Port=7777?QueryPort=27015' -server -log
```

In Docker, run the container with `--init` (Compose `init: true`).

### 3. Configure

Copy `TakaroArk/TakaroArkNative/takaro.cfg.example` to `takaro.cfg` in the same folder, set your
token and start the server:

```ini
TAKARO_REGISTRATION_TOKEN=your-registration-token-here
```

| Optional setting | What it does |
|---|---|
| `TAKARO_SERVER_NAME` | Server name in Takaro. |
| `TAKARO_SERVER_CHAT_NAME` | Sender name on messages from Takaro, used when none is set for the server in Takaro. |

Changes need a server restart.

### 4. Check that it works

Within a minute of the world loading, the console shows
`ARK_NATIVE_DIAG ... takaro-identified gameServerId=<id>` and the server shows as online in Takaro.
A wrong token shows `takaro-identify-failed`.

**Upgrading:** stop the server and unzip the new release over the old one with `unzip -o`. Do not
delete `TakaroArkNative/` first, or you lose `takaro.cfg`. Keep `<ark install>/.takaro/ark-native`;
it holds the server identity and bans.

## What works, what doesn't

Checked on server build 21241282 (game version 361.7). ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Connects once the world has loaded. |
| Player list | ✅ | |
| Player location | ✅ | |
| Teleport | ✅ | |
| Player inventory | ✅ | |
| Item list | ✅ | |
| Entity list | ✅ | |
| Locations | ⚠️ | Spawn points. Not checked yet. |
| World map | ⚠️ | The Island, after you upload a map image in Takaro. Not checked yet. |
| Send a message | ✅ | To everyone or one player. |
| Console commands | ✅ | `DoExit` is refused; use Takaro's shutdown. |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent. |
| Ban list | ⚠️ | Not checked. |
| Give an item | ✅ | |
| Shop | ✅ | Use `claim` delivery for listings. |
| Shut down the server | ✅ | Saves first. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | Global chat. |
| Player death | ✅ | |
| Entity killed | ✅ | |
| Server log lines | ✅ | Save and startup lines only. |
| Discord chat bridge | ✅ | |
| Modules | ✅ | |
| Game name, version and icon | ⚠️ | Not checked yet. |
| Other ARK maps | ⚠️ | Work, but only The Island has a map. |
| Windows servers, other builds | ❌ | The plugin refuses to load. |

Developers: the source and build scripts are in [mod/](mod/) and [scripts/](scripts/).
