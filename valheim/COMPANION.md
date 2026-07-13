# Takaro Valheim Companion

The Takaro Valheim Companion is the owned graphical-client half of the Valheim connector. It reports client-owned gameplay observations to the dedicated-server plugin over a bounded Valheim routed-RPC protocol. It does not connect to Takaro directly.

This implementation has automated, real-assembly, exact graphical-client, dedicated-server, and Takaro proof. Its client-reported inventory, chat, death, and attributed-kill paths are `live-supported` in `capabilities.json`; see [the owned-companion validation ledger](qa/2026-07-12-owned-companion-validation.md).

## Trust Boundary

No Takaro token, identity token, WebSocket URL, or other cloud credential belongs in the companion. The server registrationToken stays on the dedicated server. The companion accepts a session only from the current connected server peer and sends every report back to that exact nonzero peer; it never uses Valheim's target-zero broadcast path.

The reverse `server-chat` message is accepted only from that same authenticated server peer, current session nonce, negotiated protocol version, and increasing server sequence. The companion writes it to Valheim's normal chat history and makes the chat window visible; server messages are never rendered through the HUD overlay APIs.

Inventory, chat, death, and kill contents are client-reported and therefore untrusted. They can enrich normal community automation, but must not be treated as authoritative identity, anti-cheat, security, economy, or moderation evidence. The dedicated-server plugin binds every accepted report to the actual connected peer instead of trusting a player identity supplied by the client.

## Packages and Process Roles

- `takaro-valheim-plugin.zip` contains `TakaroValheim`, the dedicated-server connector with Takaro cloud transport and server configuration.
- `takaro-valheim-companion.zip` contains `TakaroValheimCompanion`, the graphical-client companion with no cloud transport or credentials.

Never copy `TakaroValheim.dll` into the client. Never copy `Takaro.Valheim.Companion.dll` into the dedicated server. Each plugin disables itself before Harmony setup when it detects the wrong process role.

## Install

### Dedicated server

1. Install BepInExPack Valheim on the dedicated server.
2. Extract `takaro-valheim-plugin.zip`.
3. Copy `TakaroValheim` to `BepInEx/plugins/TakaroValheim`.
4. Start once, then edit `BepInEx/config/com.takaro.valheim.cfg`.
5. Set `registrationToken`, choose `companionMode`, and restart the dedicated server.

### Graphical client

1. Install BepInExPack Valheim in the graphical Valheim client.
2. Extract `takaro-valheim-companion.zip`.
3. Copy `TakaroValheimCompanion` to `BepInEx/plugins/TakaroValheimCompanion`.
4. Start Valheim once. The optional client config is `BepInEx/config/com.takaro.valheim.companion.cfg`.
5. Keep `companionCommandPrefixes` aligned with the intended command prefixes; the default is `$`.

The client folder must contain the companion and protocol DLLs shipped together in the same archive. Do not copy a server config file into it.

## Upgrade

1. Stop the dedicated server and exit every Valheim client.
2. Replace the complete `TakaroValheim` server plugin folder from the new server ZIP.
3. Replace the complete `TakaroValheimCompanion` client plugin folder from the matching client ZIP on each participating client.
4. Delete `BepInEx/cache/chainloader_typeloader.dat` on each upgraded server or client before restarting. The release archives use deterministic timestamps, so clearing this generated cache prevents BepInEx from retaining metadata for a previous same-size DLL.
5. Keep existing BepInEx config files, review release notes for protocol/config changes, then start the server and clients.

Server and client product patch versions may differ when their wire protocol overlaps. Protocol compatibility—not an exact product-version string match—controls negotiation. Upgrade the companion when a required-mode message shows incompatible expected and actual protocol versions.

An incompatible companion reports its bounded supported protocol range to the server before required-mode disconnection, so server logs can distinguish it from a missing companion.

## Remove or Roll Back

### Remove the companion from a client

1. Exit Valheim.
2. Remove `BepInEx/plugins/TakaroValheimCompanion`.
3. Optionally remove `BepInEx/config/com.takaro.valheim.companion.cfg`.

### Remove the connector from the server

1. Stop the dedicated server.
2. Remove `BepInEx/plugins/TakaroValheim`.
3. Preserve or securely delete the server config according to your token-rotation policy.

If the server uses `companionMode=required`, removing only the client companion will intentionally prevent that client from remaining connected. Switch the server to `optional` or `disabled` before a companion rollback when uninterrupted vanilla-client access is required.

## Server Modes

- `disabled`: no companion RPC is registered and vanilla clients are unaffected.
- `optional`: compatible companions can report client-owned state; missing or expired sessions are restarted without disconnecting the player.
- `required`: a missing, incompatible, or silent companion is terminal for that connection. Initial negotiation allows 30 seconds so graphical clients can finish slow world loading. After an enforcement decision, the server revokes the session, shows a player-visible explanation, waits two seconds, sends Valheim's built-in `Kicked` RPC, and retains an exact-peer disconnect fallback.

The default is `required`. A product patch version alone does not cause rejection. Protocol v1 currently negotiates chat, inventory, player-death, entity-killed, and server-chat capabilities and uses a five-second heartbeat.

## Report Behavior

- Ordinary local chat follows normal Valheim behavior and is reported once afterward.
- Authenticated server-chat messages are added to the normal chat history as `Takaro` and are not re-emitted as player-originated inbound chat.
- An accepted configured command is reported once and suppressed from ordinary chat only after the server-bound send succeeds.
- Inventory is polled every two seconds after negotiation; the initial confirmed snapshot, including an empty inventory, is sent and unchanged canonical snapshots are not resent.
- Local player death is reported once per callback window.
- Non-player entity death is reported only when the cached last hit resolves the local player as attacker. Weapon attribution uses the equipped weapon when available and a bounded skill or `Unarmed` fallback otherwise.

All reports are bounded by the protocol schema and stop immediately when the RPC, world, server peer, or connection changes.

## Troubleshooting

- Check `BepInEx/LogOutput.log` on both roles for the plugin GUIDs `com.takaro.valheim` and `com.takaro.valheim.companion`.
- A required-mode missing-companion message means the client plugin did not complete its hello/heartbeat session.
- An incompatible message includes the server's expected protocol range and the client's actual protocol version when it was safely inspectable.
- Do not solve a version error by copying DLLs between the two role folders. Install a matching release archive for each role.
- Client reports cannot repair server-only unsupported map actions or the upstream-blocked standard `listLocations` route.
