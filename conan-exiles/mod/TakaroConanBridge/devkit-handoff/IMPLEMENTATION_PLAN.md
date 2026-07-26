# TakaroConan DevKit Implementation Plan

Create under `Content/Mods/TakaroConan`:

- `TakaroConan_ModController`
- `BP_TakaroChatCommand`
- `DT_TakaroConsoleCommands`

Map DataCmd `TakaroChat` to `BP_TakaroChatCommand`. Configure the command as a
server-executed, replicated, always-relevant DataActor owned by the addressed
player controller.

The command reads encoded message parameter 1, decodes `%20` then `%25`, and
invokes reliable owning-client RPC `ClientRenderTakaroChat`. The RPC locates
`W_ChatWindow`, creates `W_RichChatLine`, sets its `FCRichTextBlock` named
`Message`, adds the line to the scroll box, scrolls to the end, and logs
`TakaroConan: rendered `.

The pak must contain no Takaro, registration, bearer, MCP, or RCON secret. It
must perform no HTTP polling and reference no Pippi or Amunet asset or command.

Return compile/cook evidence, exact pak hash and size, schema-v2 artifact
manifest, and the four-file runtime bundle produced by the collector.
