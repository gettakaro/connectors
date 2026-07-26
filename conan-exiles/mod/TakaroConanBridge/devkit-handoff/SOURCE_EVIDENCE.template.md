# TakaroConan Source Evidence

## Build Source Identity

- Build host:
- Builder:
- Captured at UTC:
- DevKit distribution:
- DevKit branch:
- DevKit version:
- Active mod folder:
- Source workspace or commit:
- Implementation plan: IMPLEMENTATION_PLAN.md
- Source contract: BUILD_SOURCE_CONTRACT.json version 2

## Asset Source Evidence

- ModController asset path: Content/Mods/TakaroConan/TakaroConan_ModController
- DataActor command asset path: Content/Mods/TakaroConan/BP_TakaroChatCommand
- Console command table asset path: Content/Mods/TakaroConan/DT_TakaroConsoleCommands
- Evidence files or screenshots folder:

## DataCmd And Client Renderer Evidence

- [ ] The sidecar uses `con <player-index> dc TakaroChat "<sender>" "<message>"`.
- [ ] The command table maps `TakaroChat` to the command Blueprint.
- [ ] The actor is server-owned, replicated, always relevant, and controller-owned.
- [ ] Parameter 1 is decoded `%20` then `%25`.
- [ ] A reliable owning-client RPC renders through `W_ChatWindow`, `W_RichChatLine`, and `FCRichTextBlock`.
- [ ] Successful render logs `TakaroConan: rendered `.

## Inbound And Security Evidence

- [ ] Inbound chat is parsed from the vanilla dedicated-server `ChatWindow` log.
- [ ] The sidecar maps stable Steam identity before Takaro emission.
- [ ] Client assets perform no HTTP polling.
- [ ] Assets reference no Pippi or Amunet code or commands.
- [ ] Assets contain no Takaro, registration, bearer, MCP, or RCON secret.

## DevKit Compile And Cook Evidence

- Compile status:
- Cook status:
- Cooked artifact path:
- DevKit output log excerpt path:
- SHA-256 of returned `TakaroConan.pak`:
