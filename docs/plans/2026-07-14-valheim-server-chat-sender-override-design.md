# Valheim Server Chat Sender Override Design

## Goal

Render each Takaro `sendMessage` request in normal Valheim chat using that request's `opts.senderNameOverride`. The value is dynamic: `con` is only the current example, and changing it in Takaro must affect the next message without a Valheim configuration change or rebuild.

## Data flow

1. `TakaroRequestDispatcher` reads the optional `opts.senderNameOverride` string from the incoming `sendMessage` arguments.
2. The dispatcher passes the optional value through `IValheimTakaroAdapter.SendMessageAsync` to `ValheimServerAdapter`.
3. The adapter normalizes the sender once: a non-blank trimmed override is used; a missing or blank override becomes `Takaro`.
4. `CompanionServerBridge` writes that sender and the message into the authenticated `server-chat` envelope.
5. The companion keeps rendering the envelope through `Chat.instance.AddString(sender, message, Talker.Type.Normal)`.

The sender is never read from the local Valheim `serverName` setting or fetched from the Takaro gameserver record. Those values describe the server, while `senderNameOverride` describes one outbound message.

## Validation and compatibility

The sender uses the existing bounded companion payload contract. Oversized values are rejected as invalid action arguments rather than silently truncated. Missing or blank values use the stable `Takaro` fallback. Existing callers that omit `opts` retain their current appearance.

No protocol field or capability changes are required because `CompanionServerChatMessage` already carries a bounded `Sender` value.

## Verification

- Dispatcher tests prove nested `opts.senderNameOverride` reaches the adapter and missing/blank values remain optional.
- adapter/bridge contract tests prove the normalized sender replaces the hard-coded `Takaro` value.
- the complete automated suite and real server/client builds must pass.
- live proof sends one message with `senderNameOverride: "con"` and one without an override, confirming `con` and `Takaro` respectively in normal chat with no HUD overlay.
