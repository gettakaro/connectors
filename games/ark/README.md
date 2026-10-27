# Takaro ARK: Survival Evolved Connector

A server-side plugin for the **Linux** ARK: Survival Evolved dedicated server. It runs inside the server process
and connects to Takaro by itself; there is no sidecar. Players do not install anything.

## Install

### 1. Before you start

- An ARK: Survival Evolved **Linux** dedicated server on public build **21241282** (Steam app 376030), and control
  over its start command. Windows servers and other builds are not supported.
- A Takaro game server of type **Generic** and its **registration token**.

### 2. Download and unzip

Download `takaro-ark-native-linux-21241282-<version>.zip` from <https://takaro.io/connectors/ark> or the
[GitHub releases](https://github.com/gettakaro/connectors/releases?q=ark). In your ARK install folder (the one
that contains `ShooterGame/`), unzip it into a new `TakaroArk/` folder:

```sh
cd /path/to/ark && mkdir -p TakaroArk && unzip takaro-ark-native-linux-21241282-<version>.zip -d TakaroArk
```

You now have `TakaroArk/TakaroArkNative/`.

### 3. Set the registration token

Copy `TakaroArk/TakaroArkNative/takaro.cfg.example` to `takaro.cfg` in the same folder and set
`TAKARO_REGISTRATION_TOKEN`. Optional: `TAKARO_SERVER_NAME` sets the name Takaro shows. An environment variable
with the same name overrides the file.

### 4. Start the server through the launcher

Start the server with `launch.sh` instead of `ShooterGameServer`:

```sh
/path/to/ark/TakaroArk/TakaroArkNative/launch.sh /path/to/ark 'TheIsland?listen?SessionName=My-ARK?Port=7777?QueryPort=27015' -server -log
```

The launcher refuses any other ARK build. In Docker, run the container with `--init` (Compose `init: true`).

### 5. Check that it worked

Once the world has loaded (up to 15 minutes), the server's console output shows a line with
`takaro-identified gameServerId=…` and Takaro shows the server as online.

### Updating

Stop the server, unzip the new release over the old one and start it again:

```sh
cd /path/to/ark && unzip -o takaro-ark-native-linux-21241282-<version>.zip -d TakaroArk
```

Your `takaro.cfg` is kept. Do not delete `TakaroArkNative/` first. Keep `<ark install>/.takaro/ark-native`: it
holds the server identity and ban data. To uninstall, delete `TakaroArk/` and start `ShooterGameServer` directly.

## What works, what doesn't

Tested on ARK server 361.7 (Steam build 21241282) on 2026-10-26.

| Feature | Status | Note |
|---|---|---|
| Connect, reachability, reconnect after restart | ✅ | Connects once the world has loaded |
| Game profile (name, version, icon) | ✅ | Needs Takaro protocol 1. |
| Player list and lookup | ✅ | Steam ID and in-game name |
| Player location, teleport | ✅ | Matches the in-game GPS |
| Map | ⚠️ | Needs protocol v1; upload a map image in Takaro |
| Locations (spawn points) | ⚠️ | Needs Takaro protocol 1. |
| Inventory, give item | ✅ | Large amounts are delivered in several stacks |
| Item and creature lists | ✅ | Player-facing names |
| Send message, whisper | ✅ | A message to an offline player is refused |
| Console commands | ✅ | `DoExit` is refused; use Takaro's shutdown |
| Kick, ban, timed ban, unban, ban list | ✅ | Bans survive restarts |
| Shutdown | ✅ | Saves the world first |
| Join, leave, chat events | ✅ | Global chat |
| Player death event | ✅ | With position; killer only when it is a player |
| Entity killed event | ✅ | With creature and held weapon |
| Log events | ✅ | World save and startup lines only |
| Modules, shop, economy | ✅ | Use `claim` delivery for shop listings |
| Discord chat bridge | ✅ | Both ways |
| Other ARK maps | ⚠️ | Work, but only The Island has map bounds |
| Whisper to only one of two players | ⚠️ | Not tested with two players |
| Windows servers, other ARK builds | ❌ | The plugin refuses to load |
