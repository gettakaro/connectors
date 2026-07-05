# Valheim Client Command Bridge

The Valheim connector is server-side by default. The dedicated server runs
`TakaroValheim.dll` under BepInEx and connects to Takaro over the Generic
Connector WebSocket.

The optional client-side bridge is only for Takaro chat command ingress. Install
the same `TakaroValheim` BepInEx plugin folder in the Valheim client when you
need Valheim client chat commands such as `$tplist` or `$tp h` to reach Takaro.

## Client Scope

The client plugin:

- Loads under BepInEx.
- Applies a Harmony patch to `Talker.Say`.
- Reads `Takaro.clientCommandPrefixes`, default `$`.
- Sends matching command text to the dedicated server plugin with
  `TakaroClientChatCommand`.

The client plugin does not:

- Connect directly to Takaro.
- Forward inventory snapshots.
- Forward location snapshots.
- Forward player death snapshots.
- Forward entity-kill snapshots.
- Forward general non-command chat.

## Dependencies

Required on the client:

- BepInExPack Valheim.
- `0Harmony` from BepInEx.
- The repo-built `TakaroValheim.dll` and its packaged runtime dependencies.

Not required:

- Jotunn.

## Extraction Notes

If this bridge is split into a standalone Takaro-owned client mod, extract only:

- The BepInEx client entrypoint path.
- `Takaro.clientCommandPrefixes` parsing.
- The `Talker.Say` Harmony patch.
- `ForwardLocalChatCommand`.
- The server-side `TakaroClientChatCommand` RPC registration and handler.

Do not extract client inventory, location, death, entity-kill, or direct action
bridges unless that client-owned state boundary is explicitly accepted.
