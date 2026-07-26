# TakaroConan Build Report

## Build Identity

- Build host:
- Builder:
- Built at UTC:
- DevKit distribution:
- DevKit branch:
- DevKit version:
- Game branch/version:
- Source workspace or commit:
- Implementation plan: IMPLEMENTATION_PLAN.md
- Source contract: BUILD_SOURCE_CONTRACT.json

## DevKit Assets

- Active mod name: `TakaroConan`
- Active mod folder:
- ModController asset: Content/Mods/TakaroConan/TakaroConan_ModController
- DataActor command asset: Content/Mods/TakaroConan/BP_TakaroChatCommand
- Console command table asset: Content/Mods/TakaroConan/DT_TakaroConsoleCommands
- Outbound dispatch: `con <player-index> dc TakaroChat "<sender>" "<message>"`
- Client renderer: `W_ChatWindow/W_RichChatLine/FCRichTextBlock`
- Client render log prefix: `TakaroConan: rendered `
- Inbound chat source: Conan dedicated-server `ChatWindow` log
- Source evidence report: SOURCE_EVIDENCE.md

## Implementation Confirmation

- [ ] The command table maps `TakaroChat` to `BP_TakaroChatCommand`.
- [ ] The command runs server-side, replicates, is always relevant, and is owned by the addressed controller.
- [ ] The Blueprint decodes `%20` then `%25` and calls a reliable owning-client RPC.
- [ ] The RPC renders through the real Conan chat widgets and logs after success.
- [ ] Server-wide delivery addresses every online player.
- [ ] Targeted delivery addresses only the resolved player.
- [ ] Inbound chat comes from the vanilla server log with stable identity.
- [ ] Client assets perform no HTTP polling and contain no credentials.
- [ ] No Pippi or Amunet assets or commands are referenced.

## Cook Output

- Cook status:
- Output folder:
- Artifact path:
- Artifact size: <exact byte count> bytes
- SHA-256: <exact SHA-256 of TakaroConan.pak>
- DevKit output log excerpt path:

## Handoff Validation

- [ ] Intake validator passed.
- [ ] `artifact-manifest.json` attached.
- [ ] `SOURCE_EVIDENCE.md` attached.
- [ ] Report SHA-256 and size match the manifest.

## Known Deviations

List every deviation from source contract version 2.
