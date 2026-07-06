# Takaro Conan Mod Specification

## Goal

Create `TakaroConan.pak`, a minimal Conan Exiles server+client mod that replaces only the current Enhanced Pippi-backed chat/event boundary.

The existing sidecar remains the owner of Takaro WebSocket connectivity, RCON actions, save DB reads, health checks, and action/event classification. The mod must not clone Pippi features.

## Runtime Boundary

Existing sidecar endpoints:

- `GET http://127.0.0.1:3010/mod/poll`
- `POST http://127.0.0.1:3010/mod/result`
- `POST http://127.0.0.1:3010/mod/event`
- `GET http://127.0.0.1:3010/health`

The mod must run inside Conan and become the renderer/event producer for the chat boundary. The host-side Pippi poller must be stopped when the Takaro mod is under test, otherwise Pippi can mask missing Takaro mod behavior.

## Required Mod Behavior

### Startup

- Load as a Conan mod controller/component using the Conan DevKit-supported mod flow.
- Log an unmistakable startup marker, for example `TakaroConan: initialized`.
- Poll only the local sidecar URL from server runtime. Do not embed Takaro cloud tokens or RCON credentials.
- Fail visibly in Conan logs if the sidecar is unreachable.

### Poll Loop

- Poll `/mod/poll` on an interval short enough for chat to feel live, with retry/backoff when the sidecar is unavailable.
- Treat an empty poll as success and continue looping.
- For every command, call `/mod/result` with the original `requestId`.
- Only return success after the in-engine renderer accepted responsibility for rendering the message.

Expected command shape:

```json
{
  "requestId": "...",
  "action": "sendMessage",
  "args": {
    "message": "text",
    "recipient": "steam64-or-platform-id",
    "senderNameOverride": "Takaro"
  }
}
```

### Server-Wide Chat

- Render Takaro server-wide messages as normal Conan chat-feed text, not as a popup/overlay.
- Use `senderNameOverride` when present; otherwise default to `Takaro`.
- Preserve message text after sanitizing only what Conan requires for safe rendering.

Acceptance evidence:

- Takaro MCP `gameserverSendMessage` returns success.
- `/mod/result` receives success for the queued command.
- A connected player sees the line in the normal chat feed.
- Conan/server logs show the Takaro mod handled the command, not Pippi.

### Targeted Chat

- Resolve targeted recipients by stable identity first: Steam64 / platform ID.
- Fall back to current Conan character name only if stable ID is unavailable.
- Render the message only to the intended target.
- Return a structured failure when no online player matches the recipient.

Acceptance evidence:

- Takaro MCP targeted `gameserverSendMessage` returns success for an online player.
- The target player sees the message.
- A non-target player does not see the targeted message when a second client is available.

### Inbound Chat Event

- Emit inbound player chat through `/mod/event` as `chat-message`.
- Include the strongest available identity fields:
  - Steam64 / platform identifier.
  - display/player name.
  - Conan character name.
  - channel.
  - timestamp.
  - message.
- Do not duplicate the same player chat line if Conan exposes multiple hooks/log surfaces for one message.

Acceptance evidence:

- Player sends a chat line.
- Connector emits a Takaro `chat-message`.
- Takaro accepts the event without validation errors.
- The event resolves to the real Steam/platform identity, not only the character name.

## Explicit Non-Goals

- No kits, ranks, warps, currency, admin UI, data table gameplay changes, or Pippi compatibility layer.
- No Takaro cloud WebSocket client inside the `.pak`.
- No RCON password, registration token, identity token, or other secret inside the `.pak`.
- No replacement for sidecar-owned RCON actions unless Conan DevKit proves a safer in-engine route.

## Action/Event Ownership

The mod must own or improve:

- `sendMessage` rendering.
- `chat-message` emission.

The existing sidecar remains sufficient for:

- `testReachability`
- `getPlayer`
- `getPlayers`
- `executeConsoleCommand`
- `listBans`
- `kickPlayer`
- `banPlayer`
- `unbanPlayer`
- `shutdown`
- online-player `giveItem`
- online-player `teleportPlayer`

The existing sidecar/save DB remains sufficient for:

- `getPlayerLocation`
- `getPlayerInventory`
- `listItems`
- `listEntities`
- `listLocations`

Still fallback/unsupported unless a future in-engine provider is added:

- `getMapInfo`: schema fallback with `enabled=false`.
- `getMapTile`: unsupported.

## Done Criteria

The mod path is not complete until all of the following are true:

- `TakaroConan.pak` exists as a cooked Conan mod artifact.
- Server and client modlists contain `*TakaroConan.pak` and do not contain `*Pippi.pak`.
- Server and client logs prove the Takaro mod loaded.
- Host Pippi poller is stopped.
- Connector health shows `modBridge.connected=true` because the Takaro mod itself is polling.
- Server-wide chat passes with player-visible proof.
- Targeted chat passes with player-visible proof.
- Inbound chat passes with Takaro event proof.
- Existing sidecar action coverage still passes after Pippi is removed.
