# Valheim Chat Message Rendering Design

## Problem

Valheim `sendMessage` currently reports success after routing base-game `Message` and `ShowMessage` calls. Those calls render center-screen and top-left HUD overlays, not lines in the normal chat window. The connector documentation and QA evidence treated player-visible output as sufficient even though Takaro's server-message module is expected to produce chat.

The owned client companion is now required. It already authenticates the connected server, negotiates a bounded protocol, and owns client-only integrations. Rendering normal chat therefore belongs on that companion boundary.

## Decision

Route global and direct `sendMessage` deliveries through the existing companion RPC as a typed server-to-client chat message. The client accepts the message only from its active connected server and only after a negotiated companion session. It renders one normal chat line with sender `Takaro` through Valheim's `Chat.instance.AddString` API and makes the chat line visible.

`sendMessage` will no longer invoke `MessageHud.MessageType.Center`, `MessageHud.MessageType.TopLeft`, `Message`, or `ShowMessage`. HUD delivery remains available for exceptional bootstrap behavior that occurs before a companion session, such as explaining that the required companion is missing. Item-drop confirmations are outside this change.

## Protocol and Compatibility

Add an explicit server-chat payload to the versioned companion envelope and a negotiated `ServerChat` capability. A server sends chat only to a peer with an active session that negotiated that capability. A connected but outdated companion cannot silently receive an overlay fallback; the action returns an actionable incompatibility error instead of claiming delivery.

The protocol codec will bound and validate sender and message strings. The server determines the sender label (`Takaro`); clients cannot use this route to spoof server output. Existing client-to-server chat reports remain unchanged.

## Data Flow

1. Takaro sends `sendMessage`, optionally with a recipient.
2. The server resolves either the exact peer or all ready peers.
3. The companion bridge verifies each peer has a current negotiated `ServerChat` session.
4. The server encodes and sends one `server-chat` envelope through the existing routed RPC.
5. The client verifies the routed-RPC sender is its active server and validates the envelope against its negotiated session.
6. The client renders `Takaro: <message>` as `Talker.Type.Normal` in `Chat.instance` and resets the chat hide timer.
7. The adapter reports success only for actual routed companion deliveries.

## Failure Behavior

- Empty or oversized messages are rejected before routing.
- An unknown recipient returns `player_not_found`.
- A peer without a current compatible companion session returns `companion_server_chat_unavailable` for direct delivery.
- Global delivery succeeds only when at least one compatible peer receives the envelope; incompatible peers are counted as skipped and are never given overlay fallbacks.
- Invalid, stale, replayed, or non-server envelopes are ignored and logged without rendering.

## Verification

- Protocol tests cover the new type, payload validation, direction, capability, size limits, and replay/session checks.
- Client contract/tests prove authenticated server messages call `Chat.instance.AddString` with sender `Takaro` and do not call HUD APIs.
- Server/adapter tests prove direct and global routing, honest failure/count behavior, and absence of `Message`/`ShowMessage` in `sendMessage`.
- The complete Valheim test and release-package suites pass.
- Live QA installs exact packaged server and client artifacts, sends both direct and global Takaro actions, and visually confirms messages appear in chat with no center/top-left overlay.
- A real installed `serverMessages` module run or cronjob is confirmed through Takaro evidence, server logs, and the player-visible chat window.
