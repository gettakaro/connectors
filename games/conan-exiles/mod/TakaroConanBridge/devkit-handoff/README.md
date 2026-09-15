# TakaroConan DevKit Build Handoff

This folder is the source-control-safe handoff for the Conan Exiles DevKit
operator who can cook `TakaroConan.pak`.

It is not a cooked mod and it does not replace the DevKit. Conan mod assets must
be created under the DevKit active mod folder and cooked by the DevKit before
this runtime can install and live-test them.

## Target Mod

- Mod folder/name: `TakaroConan`
- Output artifact: `TakaroConan.pak`
- Required ModController asset: `TakaroConan_ModController`
- Required bridge component asset: `BP_TakaroBridgeComponent`
- Required client marker component: `BP_TakaroClientMarkerComponent`

## Implementation Files

- `TAKARO_CONAN_DEVKIT_BLUEPRINT.md`: asset graph and runtime behavior contract.
- `IMPLEMENTATION_PLAN.md`: concrete DevKit asset creation and graph checklist.
- `BUILD_SOURCE_CONTRACT.json`: machine-readable source/asset contract that
  returned evidence and `artifact-manifest.json` must reference.
- `ARTIFACT_MANIFEST.template.json`: metadata to ship with the cooked `.pak`.
- `BUILD_REPORT.template.md`: operator checklist and build evidence.
- `SOURCE_EVIDENCE.template.md`: source evidence for the actual DevKit assets
  that implement the server bridge and no-secret client marker.
- `BUILD_HOST_PREFLIGHT.ps1`: Windows/Epic DevKit host readiness check.
- `BRIDGE_CONTRACT_SMOKE.ps1`: local mock bridge for a DevKit play/session
  smoke before the cooked artifact is returned.
- `COLLECT_BUILD_ARTIFACT.ps1`: copies the cooked `.pak`, hashes it, and prepares
  a return folder.

## Non-Negotiable Boundary

The mod must replace only the Pippi-backed chat bridge behavior:

- Takaro server-wide message rendering.
- Takaro targeted message rendering.
- Inbound player chat event emission with server-known stable Steam/platform
  identity.
- Result posting for handled Takaro commands.

The mod must not clone Pippi features such as kits, economy, ranks, portals,
warps, admin UI, or gameplay systems.

The client-side part must not contain Takaro cloud tokens, registration tokens,
RCON passwords, or any server credential. The required architecture is a
server-side polling component plus a minimal no-secret client marker/load asset
so server and client both load the same Takaro-owned mod without giving the
client control of the bridge.

## Build Operator Flow

1. Install/open the Conan Exiles DevKit on a machine with enough disk.
2. Run the build-host preflight:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\BUILD_HOST_PREFLIGHT.ps1 -DevKitRoot "C:\ConanExilesDevKit"
   ```

3. Create a unique mod named `TakaroConan`.
4. Create the ModController, bridge component, and client marker described in
   `TAKARO_CONAN_DEVKIT_BLUEPRINT.md`, `IMPLEMENTATION_PLAN.md`, and
   `BUILD_SOURCE_CONTRACT.json`.
5. Compile and save all assets.
6. Before cooking, run the local bridge contract smoke while the DevKit play
   session or dedicated test session is running:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\BRIDGE_CONTRACT_SMOKE.ps1 -ExpectedPlayerId "76561198000000000" -ExpectedPlayerName "TakaroDevkitTester"
   ```

   Configure `BP_TakaroBridgeComponent` to use `http://127.0.0.1:3010` for this
   smoke. The script queues server-wide and targeted `sendMessage` commands,
   requires `source=TakaroConan` or `X-Takaro-Mod-Source: TakaroConan`, rejects
   top-level `/mod/result.success`, and requires one inbound chat event with a
   stable Steam/platform identity. Save the resulting
   `BRIDGE_CONTRACT_SMOKE.result.json` outside the runtime return bundle and
   reference it from `BUILD_REPORT.md` and `SOURCE_EVIDENCE.md`.
7. Cook/build the mod from the DevKit.
8. Fill `BUILD_REPORT.md` from the template.
9. Fill `SOURCE_EVIDENCE.md` from the template.
10. Collect the artifact:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\COLLECT_BUILD_ARTIFACT.ps1 -PakPath "C:\Path\To\TakaroConan.pak" -BuildReportPath ".\BUILD_REPORT.md" -SourceEvidencePath ".\SOURCE_EVIDENCE.md" -DevKitVersion "<version from DevKit>" -CompiledInDevKit -CookedInDevKit
   ```

11. Provide a runtime artifact folder or archive containing exactly these four
    files and no extra files: `TakaroConan.pak`, `artifact-manifest.json`,
    `BUILD_REPORT.md`, and `SOURCE_EVIDENCE.md`. Put screenshots, editor logs,
    or other supporting material outside the returned runtime bundle and
    reference them from `SOURCE_EVIDENCE.md` instead.

`COLLECT_BUILD_ARTIFACT.ps1` requires `-OutputDir` to be absent or empty and
then checks that the returned runtime bundle contains exactly those four files.
Use a fresh output directory for every build return.

This runtime will then run:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-takaro-pak.sh --pak /path/to/TakaroConan.pak --build-manifest /path/to/artifact-manifest.json
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh \
  --token-file /path/to/token \
  --artifact-from /path/to/build-host-return
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh \
  --token-file /path/to/token --apply-token \
  --artifact-from /path/to/build-host-return --apply-install \
  --stop-pippi-poller --skip-loop
# After dedicated-server restart and full client relog/rejoin:
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh \
  --post-reconnect-check
```

The combined apply command validates the token, imports the artifact, and
dry-runs the install before applying/restarting the bridge with the new token.
It only rewrites server/client modlists after artifact intake has already
passed, and fails before mutation if the Conan client is still running.
The low-level installer enforces the same client-exit rule in `--apply` mode.
