# Conan Exiles TakaroConan Final Gates

## Goal

Prepare the upstream `gettakaro/connectors` Conan Exiles connector for the final Takaro-owned Conan mod validation path. The upstream branch must contain only PR-safe Conan source, tests, scripts, and DevKit handoff documentation. Runtime-only artifacts, secrets, local logs, generated build output, and fabricated `.pak` files must not be committed.

The local `/home/hendrik/gamingconnectors` workspace remains the runtime/live-test workspace. This branch is for `/home/hendrik/connectors` upstream review and CI.

## Scope

- Port strict `/mod/event` validation for chat events.
- Require explicit TakaroConan source attribution when configured for final validation.
- Expose `/health` trace fields that prove recent `/mod/poll`, `/mod/result`, and `/mod/event` traffic source.
- Record Takaro identify failures in health and stop reconnect loops until credentials are updated.
- Add `verify:mod-protocol` as a sidecar-contract diagnostic, not installed-mod proof.
- Add focused tests for source attribution, event validation, identify failure handling, and protocol behavior.
- Add `conan-exiles/mod/TakaroConanBridge` DevKit handoff docs and templates for the external build host.

## Constraints

- Do not create a fake `TakaroConan.pak` locally.
- Do not print or commit Takaro registration tokens, identity tokens, runtime config, local logs, or generated `dist/` output.
- Treat Pippi as legacy proof and behavior reference only. The final path is a minimal Takaro-owned server+client mod for chat rendering, targeted chat, inbound chat identity, and the `/mod/poll` `/mod/result` `/mod/event` bridge contract.
- Keep final live validation separate from this upstream PR. The live runtime gates need the external DevKit artifact and a fresh out-of-band Takaro registration token.

## Verification

- `npm test` in `conan-exiles`
- `npx tsc --noEmit` in `conan-exiles`
- `npm run build` in `conan-exiles`
- `bash conan-exiles/scripts/build-release.sh`
- `npm run verify:mod-protocol` may only be used against a running disposable runtime and is not required for CI.
