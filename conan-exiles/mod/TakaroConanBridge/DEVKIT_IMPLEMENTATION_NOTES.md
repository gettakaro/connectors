# TakaroConan DevKit Implementation Notes

The validated mod uses a registered DataCmd plus a reliable client RPC. It does
not use the earlier localhost HTTP polling proposal.

`DT_TakaroConsoleCommands` registers `TakaroChat` and points at
`Local/BP_TakaroChatCommand`. Conan splits DataCmd values at ASCII spaces, so
the sidecar encodes percent as `%25` and spaces as `%20`; the Blueprint decodes
`%20` first and `%25` second.

The command actor is spawned on the server with the addressed player controller
as owner, configured to replicate and remain relevant, and then calls
`ClientRenderTakaroChat`. That reliable RPC renders through the actual Conan
chat widgets and logs `TakaroConan: rendered ` after success.

The build-host source evidence must show command-table registration, server
spawn/ownership flags, the reliable RPC, the concrete widget nodes, compile and
cook success, and absence of secrets/Pippi references.
