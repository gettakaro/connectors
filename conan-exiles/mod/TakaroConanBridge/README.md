# TakaroConan Mod Source Contract

`TakaroConan` is the Takaro-owned Conan Exiles server/client mod used only for
normal player-visible chat. The TypeScript sidecar remains responsible for
Takaro authentication, RCON, save DB reads, logs, events, and all credentials.

## Required assets

- `TakaroConan_ModController`
- `DT_TakaroConsoleCommands`
- `BP_TakaroChatCommand`

The command table registers `TakaroChat`. The sidecar addresses each recipient
with `con <player-index> dc TakaroChat <sender> <encoded-message>`. The command
actor is server-owned and replicated, then invokes a reliable owning-client RPC
that renders through `W_ChatWindow`, `W_RichChatLine`, and `FCRichTextBlock`.

Spaces are encoded as `%20`; literal percent signs are encoded as `%25` first.
The Blueprint decodes `%20` and then `%25`.

The mod contains no Takaro token, registration token, bearer token, or RCON
password. It performs no HTTP polling and references no Pippi or Amunet asset.

Inbound chat is not posted by the client mod. The sidecar parses the vanilla
dedicated-server `ChatWindow` log and maps it to stable Steam identity.

See `MOD_SPEC.md` for the runtime contract and `devkit-handoff/` for build-host
templates. `BRIDGE_CONTRACT_SMOKE.ps1` is retained only as a diagnostic for the
superseded localhost HTTP prototype; it is not a current build or proof gate.

The cooked `.pak` is a deployment artifact and is intentionally not committed.
Build/source evidence must identify the exact pak SHA-256 and byte size.
