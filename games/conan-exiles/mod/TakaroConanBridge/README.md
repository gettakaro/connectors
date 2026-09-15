# TakaroConan Bridge Handoff

This folder contains the source-control-safe handoff for a minimal Conan Exiles
server+client mod named `TakaroConan`.

The TypeScript sidecar remains responsible for Takaro WebSocket connectivity,
RCON actions, save DB reads, health checks, and action/event classification. The
mod replaces only the Pippi-backed chat bridge boundary:

- server-wide Takaro chat rendering;
- targeted Takaro chat rendering;
- inbound player chat emission through `/mod/event` with stable
  Steam/platform identity;
- `/mod/result` completion for handled sidecar commands.

The committed handoff intentionally does not include a cooked `.pak`, DevKit
cache/output, runtime config, or secrets.

## Files

- `MOD_SPEC.md`: required runtime behavior and non-goals.
- `DEVKIT_IMPLEMENTATION_NOTES.md`: implementation contract for the DevKit pass.
- `BUILD_ENVIRONMENT.md`: build/toolchain and artifact-intake boundary.
- `INSTALL_RECONNECT_LIVE_TEST.md`: final runtime replacement and validation
  runbook.
- `API_COVERAGE_BOUNDARY.md`: connector-owned Takaro action/event boundary.
- `API_GOAL_MATRIX.json`: final goal coverage matrix.
- `COMPLETION_CHECKLIST.md`: final done checklist.
- `devkit-handoff/`: Windows/Epic DevKit operator handoff scripts, templates,
  and source contract.

## Build Host Return

The DevKit build host must return exactly these files:

```text
TakaroConan.pak
artifact-manifest.json
BUILD_REPORT.md
SOURCE_EVIDENCE.md
```

No Takaro registration token, identity token, RCON password, API token, or other
secret belongs in the client-distributed `.pak` or returned runtime bundle.

## Sidecar Diagnostic

From `conan-exiles`, `npm run verify:mod-protocol` can prove the local
sidecar's `/mod/poll`, `/mod/result`, and `/mod/event` contract against a
running disposable runtime. It uses `TakaroConanProtocolProbe/1.0` and is not
installed-mod proof.
