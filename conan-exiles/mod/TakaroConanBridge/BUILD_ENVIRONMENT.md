# Takaro Conan Mod Build Environment

## Current Gate

This machine cannot currently build a cooked Conan `.pak`.

Observed on 2026-07-05:

- No Conan Enhanced DevKit root was found in known Epic/Windows/legacy Steam paths.
- No `UE4Editor`, `UnrealEditor`, `RunUAT`, `RunUAT.sh`, `UnrealPak`, or `UnrealPak.exe` found locally.
- No Conan `.uproject`, `.uplugin`, `.uasset`, or `.umap` project source under `context/games/conan-exiles`.
- Only about `79G` free on `/home/hendrik`, which is too small for a normal Conan DevKit install.
- The current Conan Exiles Enhanced DevKit is listed as an Epic Games Store Windows DevKit. Legacy Steam app `486030` is treated only as optional historical evidence.

Current source pointers:

- Epic Games Store lists `Conan Exiles Enhanced Dev Kit` as the official development kit for Conan Exiles Enhanced, Windows-only, released 2026-05-04.
- Conan Exiles wiki states that mods are built with the DevKit into a single `.pak`, can be shared directly, and that clients must restart/load the same active modlist as the server.
- Conan Mod Controller docs state that ModControllers are used to ensure a mod loads correctly and to attach components for runtime behavior.

Run:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/check-mod-toolchain.sh
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/plan-devkit-space.sh
```

## Required Build Inputs

To produce `TakaroConan.pak`, we need one of these:

1. A Windows machine with Conan Exiles Enhanced DevKit installed from Epic Games Store.
2. This machine prepared with enough disk and a working Conan DevKit/cook toolchain.
3. An already-cooked `TakaroConan.pak` from another trusted build host.

The build handoff for option 1 or 2 is in:

```text
conan-exiles/mod/TakaroConanBridge/devkit-handoff/
```

Validate that handoff before cooking:

```bash
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-devkit-handoff.sh
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/package-devkit-handoff.sh
```

When checking a build host, set the DevKit path if it is not in a known default:

```bash
TAKARO_CONAN_DEVKIT_ROOT="C:/ConanExilesDevKit" \
bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/check-mod-toolchain.sh
```

On the Windows build host, unpack the handoff archive and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\BUILD_HOST_PREFLIGHT.ps1 -DevKitRoot "C:\ConanExilesDevKit"
```

## Expected DevKit Work

Inside Conan DevKit:

1. Create a new unique mod, `TakaroConan`.
2. Add the required ModController.
3. Add the smallest server/client component or blueprint assets needed to:
   - poll `/mod/poll`;
   - render server-wide chat;
   - render targeted chat;
   - emit inbound chat to `/mod/event`;
   - post command completion to `/mod/result`.
   - load a no-secret client marker component so the client participates in the
     same Takaro-owned mod without polling the sidecar or carrying credentials.
4. Build/cook the mod.
5. Export the generated `.pak` as `TakaroConan.pak`.

## Source Control Boundary

The repo should contain:

- design/spec/runbook files;
- scripts that validate readiness and install state;
- source snippets or handoff notes safe to keep in git.

The repo should not contain:

- cooked `.pak` files unless an explicit artifact policy is added;
- DevKit cache/output directories;
- secrets;
- generated multi-GB Unreal assets.

## Artifact Intake Checklist

When a `.pak` is produced elsewhere:

```bash
sha256sum /path/to/TakaroConan.pak
ls -lh /path/to/TakaroConan.pak
```

Record:

- artifact path;
- SHA-256;
- build host;
- DevKit version/game branch;
- source revision or exported asset bundle used to build it;
- whether Pippi was absent during validation.

After receiving the artifact, use the guarded wrapper. It imports and dry-runs
the artifact first, verifies the Conan client is fully exited, then applies the
fresh token, installs the pak, stops the old Pippi/chat poller, and waits until
after server restart plus full client rejoin for the strict checkpoint:

```bash
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

The intake validator is intentionally not a load test. It rejects obvious wrong
or unsafe artifacts before install. The wrapper calls the importer and
installer, and the installer requires the returned `BUILD_REPORT.md` and
`SOURCE_EVIDENCE.md`, then cross-checks them against the exact pak and artifact
manifest. The server/client restart plus post-reconnect checkpoint proves
whether Conan actually loaded and executed the mod.
