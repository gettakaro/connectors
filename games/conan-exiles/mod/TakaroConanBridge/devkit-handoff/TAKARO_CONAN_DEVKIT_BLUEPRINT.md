# TakaroConan DevKit Blueprint Contract

This document describes the assets and runtime behavior to build inside the
Conan Exiles DevKit. It avoids C++ plugin assumptions because the Conan DevKit
workflow is asset/Blueprint based.

## Assets To Create

Create these assets under the active mod folder:

```text
Content/Mods/TakaroConan/
  TakaroConan_ModController
  BP_TakaroBridgeComponent
  BP_TakaroClientMarkerComponent
```

### `TakaroConan_ModController`

Parent class: Conan Exiles `Modcontroller`.

Responsibilities:

- Ensure the mod loads cleanly on server and client.
- Attach `BP_TakaroBridgeComponent` to the authoritative server runtime owner
  selected in the DevKit.
- Add `BP_TakaroClientMarkerComponent` as the no-secret client-side load marker
  using the correct client/server copy rule for the DevKit.

Attachment rule:

- The bridge HTTP polling component must run on the server authority only.
- Client copies must not poll the sidecar and must not contain credentials.
- Record the exact attached base blueprint/class in `BUILD_REPORT.md`.

The DevKit operator must verify the exact attach target in the current Conan
DevKit. Do not guess paths from source control; use the DevKit class picker and
record the selected class in the build report.

### `BP_TakaroBridgeComponent`

Parent class: Actor Component.

Config variables:

```text
BridgeBaseUrl = "http://127.0.0.1:3010"
PollIntervalSeconds = 0.5
SourceName = "TakaroConan"
```

Do not add Takaro cloud tokens, registration tokens, RCON passwords, or bearer
tokens to this asset.

## HTTP Contract

The component must identify itself on every `/mod/*` request using explicit
TakaroConan source attribution:

```text
X-Takaro-Mod-Source: TakaroConan
?source=TakaroConan
```

Do not rely on `User-Agent` for final source attribution. The final sidecar
strict mode ignores ambient `User-Agent` values and rejects `/mod/poll`,
`/mod/result`, and `/mod/event` requests that lack `source=TakaroConan` or
`X-Takaro-Mod-Source: TakaroConan`.

Preferred polling URL:

```text
GET http://127.0.0.1:3010/mod/poll?source=TakaroConan
```

Expected response:

```json
{
  "hasCommand": true,
  "command": {
    "requestId": "command-id",
    "action": "sendMessage",
    "args": {
      "message": "text",
      "senderNameOverride": "Takaro",
      "recipient": {
        "gameId": "76561198000735875",
        "platformId": "steam:76561198000735875",
        "name": "Limon#67642"
      }
    }
  }
}
```

Empty poll response:

```json
{
  "hasCommand": false
}
```

Result URL:

```text
POST http://127.0.0.1:3010/mod/result
X-Takaro-Mod-Source: TakaroConan
Content-Type: application/json
```

Successful result:

```json
{
  "requestId": "command-id",
  "result": {
    "success": true,
    "sent": true,
    "raw": "rendered by TakaroConan"
  }
}
```

Failed result:

```json
{
  "requestId": "command-id",
  "result": {
    "success": false,
    "error": "reason"
  }
}
```

Inbound chat event URL:

```text
POST http://127.0.0.1:3010/mod/event
X-Takaro-Mod-Source: TakaroConan
Content-Type: application/json
```

Inbound chat payload:

```json
{
  "type": "chat-message",
  "data": {
    "timestamp": "2026-07-05T12:00:00.000Z",
    "player": {
      "gameId": "76561198000735875",
      "platformId": "steam:76561198000735875",
      "name": "Limon#67642",
      "characterName": "Limon#67642"
    },
    "message": "player text",
    "channel": "Global",
    "source": "TakaroConan"
  }
}
```

## Runtime State Machine

### Begin Play

1. If not server authority, mark client component loaded and stop.
2. Set `SourceName` to `TakaroConan`.
3. Start a repeating timer with `PollIntervalSeconds`.
4. Log `TakaroConan: initialized`.

### Poll Tick

1. `GET /mod/poll?source=TakaroConan`.
2. If `hasCommand=false`, stop this tick.
3. If `hasCommand=true`, handle `command`:
   - Preserve `command.requestId` for the completion callback.
   - If `command.action != "sendMessage"`, post a failed result.
   - If no recipient is present, render server-wide normal chat.
   - If recipient is present, resolve the player by Steam/platform ID first,
     then current character/display name as fallback.
   - Render targeted normal chat only to the resolved player.
   - Post `/mod/result` with `requestId` and `result.success=true` only after render success.

### Inbound Chat

1. Hook the DevKit-supported server-side chat event.
2. Ignore messages emitted by Takaro itself if the event exposes sender metadata
   that identifies the mod/server.
3. Resolve a stable Steam/platform ID from the player/controller/state.
4. If stable Steam/platform identity is missing, do not post a `chat-message`
   event; log a local TakaroConan identity-resolution error instead.
5. POST `/mod/event` with `type="chat-message"` and the chat payload nested under `data`.

## Render Requirements

Server-wide Takaro messages:

- Must appear in the normal Conan chat feed.
- Must not use Pippi `server` RCON command.
- Must not use overlay-only broadcast behavior.

Targeted Takaro messages:

- Must appear in the target player's normal chat feed.
- Must not be emitted as a Pippi `directmessage` RCON command.
- If only one client is available, record that the one-player validation proves
  target delivery but not non-target exclusion.

Inbound player chat:

- Must reach `/mod/event`.
- Must advance `/health.modBridge.lastEventSource` to `TakaroConan`.
- Must set `/health.modBridge.lastEventType` to `chat-message`.
- Must appear in `/health.modBridge.recentEvents` with the exact message,
  `source` containing `TakaroConan`, and stable Steam/platform identity.

Outbound Takaro messages:

- Must appear in `/health.modBridge.recentResults` with the exact message,
  `action="sendMessage"`, `source` containing `TakaroConan`, and
  `resultSuccess=true`.

## Build Report Requirements

The build operator must fill:

- exact DevKit branch/version;
- active mod folder path;
- ModController asset path;
- bridge component asset path;
- exact attach target class/blueprint;
- whether the component runs server-only or server/client copies;
- cook output path;
- `TakaroConan.pak` SHA-256;
- confirmation that no Pippi assets are referenced.
- `BRIDGE_CONTRACT_SMOKE.ps1` result path and status. A passing smoke proves the
  build-host implementation polled `/mod/poll`, posted nested `/mod/result`
  payloads for server-wide and targeted commands, and posted inbound
  `/mod/event` chat with stable identity before the artifact was returned.
