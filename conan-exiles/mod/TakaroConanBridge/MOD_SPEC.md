# TakaroConan Mod Specification

## Boundary

TakaroConan replaces only the in-game chat render boundary. It does not clone
Pippi features or own Takaro connectivity.

```text
Takaro sendMessage
  -> TypeScript sidecar
  -> persistent Conan RCON connection
  -> con <player-index> dc TakaroChat <sender> <encoded-message>
  -> server-owned replicated command actor
  -> reliable owning-client RPC
  -> normal Conan chat widgets
```

Inbound chat follows a separate server-owned path:

```text
vanilla dedicated-server ChatWindow log
  -> sidecar parser and stable Steam identity mapping
  -> Takaro chat-message event
```

The legacy `/mod/poll`, `/mod/result`, and `/mod/event` helper is diagnostic and
compatibility-only. It is not the production render architecture.

## Outbound requirements

- Resolve target players from current `listplayers` data.
- Reject missing, malformed, or ambiguous targets before dispatch.
- Validate each player index as decimal digits only.
- Encode percent as `%25`, then ASCII space as `%20`.
- Reject control characters and command separators.
- Require the exact trimmed RCON response
  `Successfully executed: <exact-command>`.
- Dispatch server-wide messages once per online player.
- Record RCON dispatch acceptance separately from client delivery.
- Keep `deliveryVerified=false` until a client acknowledgement protocol exists.

## Blueprint requirements

- `DT_TakaroConsoleCommands` maps `TakaroChat` to `BP_TakaroChatCommand`.
- The DataActor runs server-side, replicates, is always relevant, and is owned
  by the addressed player controller.
- The event decodes argument index 1 and invokes a reliable owning-client RPC.
- The RPC locates the real `W_ChatWindow`, creates `W_RichChatLine`, populates
  its `FCRichTextBlock` named `Message`, adds it to the chat scroll box, and
  scrolls to the end.
- `TakaroConan: rendered ` is logged only after the line is added.

## Security

- No Takaro cloud, registration, bearer, or MCP credential in the pak.
- No RCON password in the pak.
- No client HTTP polling or sidecar-state control.
- No Pippi or Amunet dependency.
- No kits, ranks, economy, portals, warps, or admin UI.

## Acceptance

- Exact server/client pak hashes match.
- Both active modlists contain only `*TakaroConan.pak`.
- Server and client logs show the current mod mounted.
- Server-wide and targeted multiword messages visibly render.
- RCON logs show encoded DataCmd commands and no legacy renderer command.
- Inbound chat reaches Takaro with stable player identity.
- A real Takaro module command produces player-visible output.
- Automated connector tests and build pass.
