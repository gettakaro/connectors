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
- Bridge contract smoke result:

## DevKit Assets

- Active mod name: `TakaroConan`
- Active mod folder:
- ModController asset:
- Bridge component asset:
- Client marker component asset:
- Bridge component attach target:
- Component copy rule:
- Runtime source name: TakaroConan
- Bridge base URL: http://127.0.0.1:3010
- Poll endpoint: /mod/poll
- Result endpoint: /mod/result
- Event endpoint: /mod/event
- Source evidence report: SOURCE_EVIDENCE.md

## Implementation Confirmation

- [ ] `TakaroConan_ModController` exists.
- [ ] `BP_TakaroBridgeComponent` exists.
- [ ] `BP_TakaroClientMarkerComponent` exists and contains no bridge polling,
      Takaro cloud tokens, registration tokens, RCON passwords, or bearer
      tokens.
- [ ] Bridge component runs only on server authority for HTTP polling.
- [ ] Client-side assets contain no Takaro cloud tokens, registration tokens,
      RCON passwords, or bearer tokens.
- [ ] Polling uses `/mod/poll?source=TakaroConan` or
      `X-Takaro-Mod-Source: TakaroConan`.
- [ ] Results post to `/mod/result` with `source=TakaroConan` or
      `X-Takaro-Mod-Source: TakaroConan`.
- [ ] Inbound chat posts to `/mod/event` with `source=TakaroConan` or
      `X-Takaro-Mod-Source: TakaroConan`.
- [ ] The implementation does not rely on `User-Agent` for bridge source
      attribution.
- [ ] Server-wide messages render in normal Conan chat.
- [ ] Targeted messages render to the target player.
- [ ] Inbound player chat includes stable Steam/platform identity before
      posting `chat-message` to `/mod/event`.
- [ ] No Pippi/Amunet assets or RCON chat commands are referenced.
- [ ] `IMPLEMENTATION_PLAN.md` was followed, or every deviation is listed below.
- [ ] `BUILD_SOURCE_CONTRACT.json` was followed, or every deviation is listed
      below.
- [ ] `SOURCE_EVIDENCE.md` is attached and documents the ModController,
      server bridge component, no-secret/non-polling client marker, compile
      proof, cook proof, and no-Pippi/no-secret checks.
- [ ] `BRIDGE_CONTRACT_SMOKE.ps1` passed against a DevKit play/session or
      dedicated test session, or every failure is listed under Known Deviations.
- [ ] The bridge smoke result shows server-wide `/mod/result`, targeted
      `/mod/result`, and inbound `/mod/event` evidence with explicit
      TakaroConan source attribution and nested result payloads.

## Cook Output

- Cook status:
- Output folder:
- Artifact path:
- Artifact size: <exact byte count> bytes
- SHA-256: <exact SHA-256 of TakaroConan.pak>
- DevKit output log excerpt path:

## Handoff Validation

Run before sending the artifact:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-takaro-pak.sh --pak /path/to/TakaroConan.pak --build-manifest artifact-manifest.json --manifest intake-manifest.json
```

- [ ] Intake validator passed.
- [ ] `artifact-manifest.json` attached.
- [ ] `SOURCE_EVIDENCE.md` attached.
- [ ] This report's SHA-256 and artifact size match `artifact-manifest.json`.
- [ ] This build report attached.

## Known Deviations

List any deviation from `TAKARO_CONAN_DEVKIT_BLUEPRINT.md`,
`IMPLEMENTATION_PLAN.md`, or `BUILD_SOURCE_CONTRACT.json` and why it is safe.
