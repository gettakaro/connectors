# Takaro Project Zomboid Connector

A server-side Java agent (`-javaagent`) loaded into the Project Zomboid Build 42
dedicated server JVM. It opens an outbound WebSocket to Takaro, hooks game methods
for events, and runs actions on the game main thread. All 17 Takaro actions and 6
events are implemented; 22/23 hard-verified live (see the campaign evidence in
`gamingconnectors/context/games/project-zomboid/`), `listLocations` partial (B42 has
no named-location registry).

## Install

Drop `TakaroConnector.jar` in the server cache dir (`<cachedir>/Takaro/`) and inject it
into the server JVM. Injection paths, in order of preference:

1. **`JAVA_TOOL_OPTIONS`** env var (Docker/self-hosted): `-javaagent:/home/steam/Zomboid/Takaro/TakaroConnector.jar`. Zero file edits; survives SteamCMD validate (which only touches the install dir, not the cache dir).
2. `vmArgs` in `ProjectZomboid64.json` — works, but SteamCMD validate reverts it; re-apply after updates.
3. `-javaagent:<jar> --` as a launch option before the `--` separator.

Config: `<cachedir>/Takaro/TakaroConfig.txt` (`key=value`), overridden by `TAKARO_*` env vars.
Keys: `wsUrl`, `identityToken`, `registrationToken`, `debug`, `logEvents`, `serverChatName`.

## Chat sender name

Messages the connector sends to game chat are prefixed with a sender name, resolved:

1. Takaro's `opts.senderNameOverride` on the `sendMessage` action (Takaro's own
   Server Chat Name setting, when it attaches it), else
2. the connector's `serverChatName` config (`serverChatName` / env `TAKARO_SERVER_CHAT_NAME`), else
3. the live PZ server name (`GameServer.serverName`), else `"Server"`.

### KNOWN ISSUE — still to change

Takaro does **not** currently attach the domain/gameserver **Server Chat Name** setting
to admin/dashboard messages: every `sendMessage` observed over the wire (raw `/message`
API and dashboard chat, before and after a server restart) arrives with `opts={}` — no
`senderNameOverride`. Takaro also sends the connector no settings on identify (only
`gameServerId`), so the connector cannot read the setting itself. As a result, admin
messages currently fall back to the real server name (e.g. `Takaro Dev Zomboid: ...`)
instead of the configured Server Chat Name.

**For now** we accept the server-name fallback. **To fix properly**, Takaro should attach
`serverChatName` as `senderNameOverride` on admin/dashboard `sendMessage` (it appears to
do so only for module/command flows) — this is a Takaro-side change to report. Setting the
connector's own `TAKARO_SERVER_CHAT_NAME` is a stopgap but duplicates Takaro's setting, so
it is intentionally left unset.
