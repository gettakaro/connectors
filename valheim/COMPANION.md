# Takaro Valheim Companion

The Takaro Valheim Companion is the owned graphical-client half of the Valheim connector. It reports client-owned gameplay observations to the dedicated-server plugin over a bounded Valheim routed-RPC protocol. It does not connect to Takaro directly.

This implementation has automated, real-assembly, exact graphical-client, dedicated-server, and Takaro proof. Its client-reported inventory, chat, death, and attributed-kill paths are `live-supported` in `capabilities.json`; see [the owned-companion validation ledger](qa/2026-07-12-owned-companion-validation.md).

## Trust Boundary

No Takaro token, identity token, WebSocket URL, or other cloud credential belongs in the companion. The server registrationToken stays on the dedicated server. The companion accepts a session only from the current connected server peer and sends every report back to that exact nonzero peer; it never uses Valheim's target-zero broadcast path.

The reverse `server-chat` message is accepted only from that same authenticated server peer, current session nonce, negotiated protocol version, and increasing server sequence. The companion writes it to Valheim's normal chat history and makes the chat window visible; server messages are never rendered through the HUD overlay APIs. Takaro's `opts.senderNameOverride` is dynamic per message, and a missing or blank value displays as `Takaro`.

Item grants run the other way: the server instructs the companion to place items in the
local player's inventory. The companion inserts what fits and drops the remainder in the
world, so a full inventory cannot swallow a grant. This does not weaken the rule below —
the server decides what is granted and the client only carries it out, while everything
the client *reports* stays untrusted.

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

An incompatible companion reports its bounded supported protocol range to the server before required-mode disconnection, so server logs can distinguish it from a missing companion. **This holds only for a companion new enough to parse the server's hello — protocol 2 or later.** A protocol-1 companion cannot parse a protocol-2 hello, because it rejects the unknown `ItemGrant` capability bit, so it answers nothing and the server reports it as `reason=MissingCompanion, expected=2, actual=missing`. An out-of-date companion of that vintage is still indistinguishable from an absent one, because the server never receives a version it could report. What the server can do is name both possibilities, and it now does: the enforcement line appends `No companion answered the hello: none is installed, or it is older than protocol 2 and cannot read it.`, and the player-visible explanation likewise says the companion is either not installed or older than the minimum protocol and should be installed or updated. The companion itself still logs no diagnostic of its own — a version too old to parse the hello is also too old to know it failed. Both the original observation and the improved wording were proven live on 2026-09-02 — see [the item-grant validation ledger](qa/2026-09-02-item-grant-validation.md).

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

The default is `required`. A product patch version alone does not cause rejection. Protocol v2 negotiates chat, inventory, player-death, entity-killed, server-chat, and item-grant capabilities and uses a five-second heartbeat.

## Observed Mode Behaviour

The three modes were exercised live on 2026-09-02 against a real dedicated server and a
real graphical client; see [the acceptance ledger](qa/2026-09-02-acceptance-validation.md).

| Mode | Companion RPC | A player with no companion | What the server loses |
| --- | --- | --- | --- |
| `disabled` | never registered | joins and stays | inventory, inbound chat, death, kills, and outbound `sendMessage` |
| `optional` | registered | joins and stays, indefinitely | those capabilities, for that player only |
| `required` | registered | disconnected after the grace period | nothing |

Under `required`, the server logs the decision before acting:

```text
required companion enforcement scheduled for peer <id>:
  reason=MissingCompanion, expected=2, actual=missing. No companion answered the
  hello: none is installed, or it is older than protocol 2 and cannot read it.
sent the built-in kicked RPC to peer <id> after the companion explanation grace period.
```

`expected` is the server's own protocol version, so it reads `2` on the current release
and read `1` when this was first observed on the protocol-1 artifact.

The player sees Valheim's own "You have been kicked from the server." dialog, so the
disconnect is never silent. Under `optional` the same unanswered handshake produces no
enforcement line at all.

Choose `optional` when you cannot guarantee every player installs the companion but still
want Takaro's server-owned features for everyone; choose `required` only when you can
actually distribute the companion.

## Report Behavior

- Ordinary local chat follows normal Valheim behavior and is reported once afterward.
- Authenticated server-chat messages are added to the normal chat history under that request's bounded sender override, or `Takaro` when missing, and are not re-emitted as player-originated inbound chat.
- An accepted configured command is reported once and suppressed from ordinary chat only after the server-bound send succeeds.
- Inventory is polled every two seconds after negotiation; the initial confirmed snapshot, including an empty inventory, is sent and unchanged canonical snapshots are not resent.
- An accepted item grant is applied immediately and forces the next snapshot, so Takaro's inventory view reflects the grant without waiting for ordinary change detection. The player is told in chat how much entered their inventory and how much was dropped.
- Local player death is reported once per callback window.
- Non-player entity death is reported only when the cached last hit resolves the local player as attacker. Weapon attribution uses the equipped weapon when available and a bounded skill or `Unarmed` fallback otherwise.

All reports are bounded by the protocol schema and stop immediately when the RPC, world, server peer, or connection changes.

## Troubleshooting

- Check `BepInEx/LogOutput.log` on both roles for the plugin GUIDs `com.takaro.valheim` and `com.takaro.valheim.companion`.
- A required-mode missing-companion message means the client plugin did not complete its hello/heartbeat session.
- An incompatible message includes the server's expected protocol range and the client's actual protocol version when it was safely inspectable.
- Do not solve a version error by copying DLLs between the two role folders. Install a matching release archive for each role.
- Client reports cannot repair server-only unsupported map actions or the upstream-blocked standard `listLocations` route.
