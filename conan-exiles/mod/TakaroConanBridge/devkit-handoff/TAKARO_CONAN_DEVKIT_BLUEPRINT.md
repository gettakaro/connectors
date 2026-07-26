# TakaroConan DevKit Blueprint Contract

```text
Content/Mods/TakaroConan/
  TakaroConan_ModController
  DT_TakaroConsoleCommands
  BP_TakaroChatCommand
```

The ModController registers the command table. The table maps `TakaroChat` to
the command DataActor.

```text
sidecar RCON: con <player-index> dc TakaroChat <sender> <encoded-message>
  -> server-owned replicated command actor
  -> reliable owning-client RPC
  -> W_ChatWindow / W_RichChatLine / FCRichTextBlock
```

The sidecar encodes percent as `%25`, then spaces as `%20`. The Blueprint
decodes `%20` and then `%25`. The command actor uses the addressed controller as
owner and logs `TakaroConan: rendered ` only after adding the visible chat line.

The pak has no credentials, performs no HTTP calls, and has no Pippi/Amunet
dependency. Client render must be proved from the client log and visible chat,
not inferred from RCON acceptance.
