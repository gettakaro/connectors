# Takaro ARK: Survival Evolved Connector

A native plugin for the **Linux** ARK: Survival Evolved dedicated server. It runs inside the game
server and connects to Takaro by itself, with no sidecar. Players do not install anything.

## Install

### 1. Before you start

- An ARK: Survival Evolved **Linux** dedicated server on public build **21241282** (Steam app
  376030). Windows servers and other builds are not supported.
- Control over the server's start command.
- A Takaro game server of type **Generic** and its **registration token** (Takaro → Game servers →
  Add → Generic).

### 2. Download

Download the latest release from https://takaro.io/connectors/ark (or from the releases page,
https://github.com/gettakaro/connectors/releases?q=ark). Take
`takaro-ark-native-linux-21241282-<version>.zip`. Check it with `SHA256SUMS` from the same release
(`sha256sum -c SHA256SUMS --ignore-missing`).

### 3. Install

Stop the server. In your ARK install folder (the one that contains `ShooterGame/`), make a folder
`TakaroArk/` and unzip into it:

```sh
cd /path/to/ark && mkdir -p TakaroArk && unzip takaro-ark-native-linux-21241282-<version>.zip -d TakaroArk
```

```
<ark install>/
    ShooterGame/
    TakaroArk/
        TakaroArkNative/
            libtakaro-ark-native.so
            launch.sh
            takaro.cfg.example
            ...
```

From now on, start the server through the launcher instead of `ShooterGameServer`:

```sh
/path/to/ark/TakaroArk/TakaroArkNative/launch.sh /path/to/ark 'TheIsland?listen?SessionName=My-ARK?Port=7777?QueryPort=27015' -server -log
```

The launcher checks the server executable and refuses any other ARK build. In Docker, run the
container with `--init` (Compose `init: true`).

### 4. Configure

Copy `TakaroArk/TakaroArkNative/takaro.cfg.example` to `takaro.cfg` in the same folder, set
`TAKARO_REGISTRATION_TOKEN` and start the server. The other keys are optional:

| Key | Default |
|---|---|
| `TAKARO_SERVER_NAME` | `ARK (<first characters of the identity>)`; unique in your Takaro domain. |
| `TAKARO_IDENTITY_TOKEN` | Generated once and kept in the state folder. |
| `TAKARO_WS_URL` | `wss://connect.takaro.io/` |
| `TAKARO_STATE_DIR` | `<ark install>/.takaro/ark-native` |
| `TAKARO_CA_FILE` | The system CA bundle. |
| `TAKARO_SERVER_CHAT_NAME` | None. Sender name on messages from Takaro (see below). |

A non-empty environment variable with the same name wins over the file.

**Server chat name (optional).** Messages from Takaro show the sender in front of the text, in ARK's chat sender colour (`Takaro: Welcome!`), in global chat and to one player. The sender is the chat name set for this server in Takaro; a change there applies to the next message, no restart needed. On a Takaro version that does not send that name, set `TAKARO_SERVER_CHAT_NAME` in `takaro.cfg` instead (read at server start); the name set in Takaro wins over it. With neither, the message shows as ARK's own `(SERVER): <message>` line.

### 5. Check that it works

The server shows up as online in Takaro within a minute of the world loading (at most 15 minutes
after start). The server's console output shows:

```
ARK_NATIVE_DIAG ... takaro-identified gameServerId=<id> protocolVersion=<n>
```

A wrong token shows `takaro-identify-failed` instead. Fix `takaro.cfg` and restart the server.

### 6. Upgrade

Stop the server, unzip the new release over the old one and start the server again:

```sh
cd /path/to/ark && unzip -o takaro-ark-native-linux-21241282-<version>.zip -d TakaroArk
```

The zip has no `takaro.cfg`, so yours stays; do not delete `TakaroArkNative/` first, or you lose
it. Keep `<ark install>/.takaro/ark-native` (or your `TAKARO_STATE_DIR`): it holds the server
identity, ban reasons and expiry dates. To uninstall, stop the server, delete `TakaroArk/` and
start `ShooterGameServer` directly again.

## What works, what doesn't

Checked on server build 21241282 (game version 361.7). ✅ works, ⚠️ works with a limit, ❌ not supported.

| What | | Notes |
|---|---|---|
| Connection | ✅ | Connects once the world has loaded. |
| Player list | ✅ | Steam ID and in-game name. |
| Player location | ✅ | Matches the in-game GPS. |
| Teleport | ✅ | |
| Player inventory | ✅ | |
| Item list | ✅ | Player-facing names. |
| Entity list | ✅ | Player-facing creature names. |
| Locations | ⚠️ | Spawn points. Not checked on takaro.io yet. |
| World map | ⚠️ | The Island, after you upload a map image in Takaro. Not checked on takaro.io yet. |
| Send a message | ✅ | To everyone or one player, with the server chat name as sender; a message to an offline player is refused, never broadcast. |
| Console commands | ✅ | `DoExit` is refused; use Takaro's shutdown. |
| Kick | ✅ | |
| Ban and unban | ✅ | Timed and permanent; bans survive restarts and timed bans are lifted by the plugin. |
| Ban list | ⚠️ | Not checked. |
| Give an item | ✅ | Amounts above one stack are delivered in rounds. |
| Shop | ✅ | Use `claim` delivery for shop listings. |
| Shut down the server | ✅ | Saves the world first. |
| Player joined / left | ✅ | |
| Chat messages | ✅ | Global chat. |
| Player death | ✅ | With position; the attacker only when the killer is a player. |
| Entity killed | ✅ | The creature and the held weapon (empty when unarmed). |
| Server log lines | ✅ | World save and startup lines only. |
| Discord chat bridge | ✅ | |
| Modules | ✅ | |
| Game name, version and icon | ⚠️ | Shows ARK: Survival Evolved, the game build, the connector version and the game icon. Not checked on takaro.io yet. |
| Other ARK maps | ⚠️ | Work, but only The Island has map bounds. |
| Windows servers, other builds | ❌ | The plugin refuses to load on any other executable. |

Developers: the source and build scripts are in [mod/](mod/) and [scripts/](scripts/).
