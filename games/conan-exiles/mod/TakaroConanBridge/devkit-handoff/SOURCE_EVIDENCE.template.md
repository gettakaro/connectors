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
- Source contract: BUILD_SOURCE_CONTRACT.json
- Bridge contract smoke result:

## Asset Source Evidence

- ModController asset path:
- Bridge component asset path:
- Client marker component asset path:
- Bridge component attach target:
- Component copy rule:
- Evidence files or screenshots folder:
- Implementation plan evidence:
- Source contract evidence:

## Server Authority Bridge Evidence

- [ ] `BP_TakaroBridgeComponent` exists in the TakaroConan mod folder.
- [ ] The bridge component runs HTTP polling only on server authority.
- [ ] The bridge component polls `/mod/poll` with
      `X-Takaro-Mod-Source: TakaroConan` or `source=TakaroConan`.
- [ ] The bridge component posts command results to `/mod/result` with
      `X-Takaro-Mod-Source: TakaroConan` or `source=TakaroConan`.
- [ ] The bridge component posts inbound player chat to `/mod/event` with
      `X-Takaro-Mod-Source: TakaroConan` or `source=TakaroConan`.
- [ ] The bridge component does not rely on `User-Agent` for source
      attribution.
- [ ] Server-wide Takaro messages render in normal Conan chat.
- [ ] Targeted Takaro messages render to the target player in normal Conan chat.
- [ ] Inbound player chat includes stable Steam/platform identity before posting
      `chat-message` to `/mod/event`.
- [ ] `BRIDGE_CONTRACT_SMOKE.ps1` passed or every failure is listed in
      `BUILD_REPORT.md`.
- [ ] The bridge smoke result includes server-wide command result, targeted
      command result, and inbound chat event proof.

## Client Marker Evidence

- [ ] `BP_TakaroClientMarkerComponent` exists in the TakaroConan mod folder.
- [ ] The client marker contains no Takaro cloud token.
- [ ] The client marker contains no registration token.
- [ ] The client marker contains no RCON password.
- [ ] The client marker contains no bearer token.
- [ ] The client marker does not call `/mod/poll`.
- [ ] The client marker does not call `/mod/result`.
- [ ] The client marker does not call `/mod/event`.

## No Pippi Or Secret Evidence

- [ ] The TakaroConan assets do not reference Pippi assets.
- [ ] The TakaroConan assets do not reference Amunet assets.
- [ ] The TakaroConan implementation does not use Pippi RCON chat commands.
- [ ] `IMPLEMENTATION_PLAN.md` was followed, or deviations are listed in
      `BUILD_REPORT.md`.
- [ ] `BUILD_SOURCE_CONTRACT.json` was followed, or deviations are listed in
      `BUILD_REPORT.md`.
- [ ] The source evidence bundle contains no Takaro cloud tokens, registration
      tokens, RCON passwords, bearer tokens, or copied secrets.

## DevKit Compile And Cook Evidence

- Compile status:
- Cook status:
- Cooked artifact path:
- DevKit output log excerpt path:
- SHA-256 of returned `TakaroConan.pak`:
