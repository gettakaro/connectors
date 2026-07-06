# TakaroConan DevKit Implementation Plan

This plan turns `TAKARO_CONAN_DEVKIT_BLUEPRINT.md` into concrete DevKit asset
work. The build operator must follow it, document any deviation in
`BUILD_REPORT.md`, and return proof in `SOURCE_EVIDENCE.md`.

## Build Host Inputs

- Open the Conan Exiles DevKit on the build host.
- Create or open the active mod named `TakaroConan`.
- Use bridge base URL `http://127.0.0.1:3010`.
- Use runtime source name `TakaroConan`.
- Do not add Takaro cloud tokens, registration tokens, RCON passwords, bearer
  tokens, Pippi assets, or Amunet assets to any asset, config, text field, or
  screenshot evidence.

## Asset Creation Tasks

- Create `TakaroConan_ModController` in the `TakaroConan` mod folder.
- Create `BP_TakaroBridgeComponent` in the `TakaroConan` mod folder.
- Create `BP_TakaroClientMarkerComponent` in the `TakaroConan` mod folder.
- Attach or instantiate `BP_TakaroBridgeComponent` only from server-authoritative
  runtime flow owned by `TakaroConan_ModController`.
- Attach or include `BP_TakaroClientMarkerComponent` only as a client-load marker
  so clients load the same Takaro-owned mod package without owning the bridge.

## ModController Graph Tasks

- On Begin Play, check server authority before starting bridge work.
- On server authority, create or attach one `BP_TakaroBridgeComponent` instance.
- Do not create bridge polling components on client-only copies.
- Keep the client marker separate from the server bridge.
- Expose or document the exact attach/copy rule in `SOURCE_EVIDENCE.md`.

## Server Bridge Component Graph Tasks

- Begin Play graph: initialize the bridge only when running with server
  authority.
- Poll graph: call `/mod/poll?source=TakaroConan` or call `/mod/poll` with
  `X-Takaro-Mod-Source: TakaroConan`.
- Result graph: when a command has `requestId`, post completion to
  `/mod/result` with explicit `source=TakaroConan` or
  `X-Takaro-Mod-Source: TakaroConan` and this exact body shape:
  `{ "requestId": "<id>", "result": { "success": true, "sent": true } }`.
  Do not put `success` at the top level; the sidecar reads `body.result`.
- Result failure graph: for rejected, unknown, or failed commands, post
  `{ "requestId": "<id>", "result": { "success": false, "error": "<reason>" } }`.
- Inbound chat graph: when normal Conan player chat is observed, post to
  `/mod/event` with stable Steam/platform player identity, player display name,
  message, and explicit `source=TakaroConan` or
  `X-Takaro-Mod-Source: TakaroConan`.
- Do not rely on `User-Agent` for `/mod/*` source attribution; final strict
  sidecar mode ignores it.
- Server-wide chat command graph: render Takaro server-wide messages in normal
  Conan chat, not as overlay-only UI.
- Targeted chat command graph: render Takaro targeted messages to the selected
  player in normal Conan chat.
- Failure graph: report rejected, unknown, or failed commands through the
  nested `/mod/result` payload above.

## Client Marker Component Tasks

- Include `BP_TakaroClientMarkerComponent` in the mod so clients can join a
  server that requires `TakaroConan.pak`.
- Keep the marker no-secret and non-polling.
- The client marker must not call `/mod/poll`, `/mod/result`, or `/mod/event`.
- The client marker must not send Takaro actions, receive Takaro commands, or
  control bridge state.
- The client marker may only prove client package load/compatibility.

## No-Pippi Requirements

- The implementation must not reference Pippi assets.
- The implementation must not reference Amunet assets.
- The implementation does not use Pippi RCON chat commands.
- Chat delivery must come from Takaro-owned DevKit assets and sidecar
  `/mod/*` endpoints, not from host Pippi log polling.
- Any unavoidable engine-level limitation must be listed as a deviation in
  `BUILD_REPORT.md` before the artifact can be imported.

## Required Build-Host Proof

- Before returning the artifact, run `BRIDGE_CONTRACT_SMOKE.ps1` against a DevKit
  play/session or dedicated test session that loads `TakaroConan`. Keep
  `BridgeBaseUrl` set to `http://127.0.0.1:3010` for this smoke. The smoke must
  pass or every failure must be listed as a deviation in `BUILD_REPORT.md`.
  Reference the generated `BRIDGE_CONTRACT_SMOKE.result.json` from
  `BUILD_REPORT.md` and `SOURCE_EVIDENCE.md`; do not put it in the four-file
  runtime return bundle.
- `BUILD_REPORT.md` must confirm this implementation plan was followed or list
  every deviation.
- `BUILD_REPORT.md` and `SOURCE_EVIDENCE.md` must reference
  `BUILD_SOURCE_CONTRACT.json` and confirm that its required asset/runtime
  proof was followed or list every deviation.
- `SOURCE_EVIDENCE.md` must name this implementation plan and include evidence
  for the ModController graph, server bridge component graph, client marker
  component, no-Pippi checks, compile, cook, artifact SHA-256, and artifact size.
- `artifact-manifest.json` must set `source.implementationPlan` to
  `IMPLEMENTATION_PLAN.md`, `source.sourceContract` to
  `BUILD_SOURCE_CONTRACT.json`, and `sourceEvidence.sourceContract` to
  `BUILD_SOURCE_CONTRACT.json`.
- The returned runtime-installable bundle must include exactly one each of
  `TakaroConan.pak`, `artifact-manifest.json`, `BUILD_REPORT.md`, and
  `SOURCE_EVIDENCE.md`, and no other files. Reference screenshots or editor log
  paths from `SOURCE_EVIDENCE.md`; do not put them inside the runtime artifact
  bundle.
- Run `COLLECT_BUILD_ARTIFACT.ps1` with a fresh absent or empty `-OutputDir`.
  The collector must fail if the output directory already contains stale files
  and must verify the final four-file runtime bundle before reporting ready.
