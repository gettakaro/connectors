# TakaroConan DevKit Build Handoff

This folder defines the source and evidence contract for the Takaro-owned Conan
server/client mod. It is not a cooked mod and does not replace the DevKit.

Required assets are `TakaroConan_ModController`, `BP_TakaroChatCommand`, and
`DT_TakaroConsoleCommands`. Follow `TAKARO_CONAN_DEVKIT_BLUEPRINT.md`,
`IMPLEMENTATION_PLAN.md`, and `BUILD_SOURCE_CONTRACT.json`; compile and cook;
complete the build/source evidence templates; then run
`COLLECT_BUILD_ARTIFACT.ps1` with a fresh output directory.

The returned runtime bundle contains exactly `TakaroConan.pak`,
`artifact-manifest.json`, `BUILD_REPORT.md`, and `SOURCE_EVIDENCE.md`.

The client asset performs no HTTP polling and contains no credential. The
legacy `BRIDGE_CONTRACT_SMOKE.ps1` is diagnostic-only and is not a current
build or acceptance gate.
