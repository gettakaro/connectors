# DevKit Implementation Notes

These notes are for the Conan DevKit implementation pass. They are not a substitute for a cooked `.pak`.

Build one narrow Conan mod named `TakaroConan`.

The mod should create one runtime component/controller that:

- starts when the dedicated server loads the mod;
- logs `TakaroConan: initialized`;
- repeatedly polls `http://127.0.0.1:3010/mod/poll?source=TakaroConan`;
- sends `X-Takaro-Mod-Source: TakaroConan` on `/mod/poll`, `/mod/result`, and `/mod/event`;
- renders only `sendMessage` commands;
- posts command results to `http://127.0.0.1:3010/mod/result`;
- emits inbound chat to `http://127.0.0.1:3010/mod/event`;
- never stores Takaro secrets or RCON credentials in cooked client-visible assets.

Poll response:

```json
{
  "hasCommand": true,
  "command": {
    "requestId": "uuid",
    "action": "sendMessage",
    "args": {
      "message": "text",
      "recipient": "steam64-or-platform-id",
      "senderNameOverride": "Takaro"
    }
  }
}
```

Result response:

```json
{
  "requestId": "uuid",
  "result": {
    "success": true,
    "sent": true,
    "mod": "TakaroConan"
  }
}
```

Server-wide messages must render into the normal chat feed and must not use overlay-only broadcast behavior.

Targeted messages must resolve recipient by Steam64/platform ID first, then fall back to current character/player name only when stable ID is unavailable.

Inbound chat events must include the strongest identity Conan exposes from the chat hook: Steam/platform ID, display/player name, character name, channel, timestamp, and message.

After outbound messages, `/health.modBridge.recentResults` must include the
exact message text, `action="sendMessage"`, `source` containing
`TakaroConan`, and `resultSuccess=true`.

After an inbound event, `/health` must show `modBridge.lastEventSource`
containing `TakaroConan`, `modBridge.lastEventType="chat-message"`, and
`modBridge.recentEvents` containing the exact inbound message with stable
Steam/platform identity.
