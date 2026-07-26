# Takaro Conan Mod Completion Checklist

> Historical checklist: references to a server HTTP polling component or
> no-op client marker are superseded by the schema-v2 DataCmd/client-renderer
> contract in `MOD_SPEC.md` and `devkit-handoff/BUILD_SOURCE_CONTRACT.json`.

This checklist is the final gate for saying the Takaro Conan server+client mod goal is done.

For the currently verified live blockers and external inputs needed before the
install window, see `CURRENT_BLOCKERS_AND_REQUIRED_INPUTS.md`.

## Build Gate

- [ ] Conan Exiles Enhanced DevKit is installed locally or on the build machine; current expected source is the Epic Games Store Windows DevKit, while Steam app `486030` is only legacy/historical evidence.
- [ ] Unreal editor, `RunUAT`, and `UnrealPak` are available for the Conan DevKit version.
- [ ] Unreal/Conan mod project assets for `TakaroConan` exist.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-devkit-handoff.sh` passes for the source-control handoff.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/package-devkit-handoff.sh` produces the build-host handoff archive.
- [ ] Windows build host preflight `BUILD_HOST_PREFLIGHT.ps1` passes.
- [ ] `TakaroConan.pak` is cooked from those assets.
- [ ] The `.pak` contains no Takaro cloud tokens, registration tokens, RCON passwords, or identity tokens.
- [ ] Returned build-host artifact includes `TakaroConan.pak`, `artifact-manifest.json`, `BUILD_REPORT.md`, and `SOURCE_EVIDENCE.md`.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-build-report.sh --report /path/to/BUILD_REPORT.md --pak /path/to/TakaroConan.pak --build-manifest /path/to/artifact-manifest.json` passes with no unchecked report items and exact pak SHA-256/size match.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-source-evidence.sh --evidence /path/to/SOURCE_EVIDENCE.md --build-manifest /path/to/artifact-manifest.json` passes and proves the server bridge plus no-secret/non-polling client marker source evidence.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/import-takaro-artifact.sh --from /path/to/build-host-return` passes.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh --token-file /path/to/token --artifact-from /path/to/build-host-return` passes in dry-run mode before any runtime writes.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh --token-file /path/to/token --apply-token --artifact-from /path/to/build-host-return --apply-install --stop-pippi-poller --skip-loop` applies the token/artifact without running final proof before restart/rejoin, and fails before token apply or client modlist mutation if the Conan client is still running.
- [ ] Direct `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/install-takaro-mod.sh --pak /path/to/TakaroConan.pak --apply` is not used to bypass the wrapper; if used, it also fails while the Conan client is still running.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-takaro-pak.sh --pak /path/to/TakaroConan.pak --build-manifest /path/to/artifact-manifest.json` passes for the cooked artifact.
- [ ] If the cooked pak has opaque metadata, the intake manifest records `markerEvidence=build-manifest` and the build manifest SHA-256/size match the pak.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/check-mod-toolchain.sh` passes on the build machine.

## Install And Reconnect Gate

- [ ] Copy `TakaroConan.pak` to the server Conan `Mods` directory.
- [ ] Copy `TakaroConan.pak` to the local client Conan `Mods` directory used for the live test.
- [ ] Preserve `TakaroConan.artifact-manifest.json` next to the installed pak on server and client.
- [ ] Preserve `TakaroConan.BUILD_REPORT.md` next to the installed pak on server and client.
- [ ] Preserve `TakaroConan.SOURCE_EVIDENCE.md` next to the installed pak on server and client.
- [ ] Server/client `TakaroConan.pak` SHA-256 values match each other.
- [ ] Server/client `TakaroConan.artifact-manifest.json` SHA-256 values match each other.
- [ ] Server/client `TakaroConan.BUILD_REPORT.md` SHA-256 values match each other.
- [ ] Server/client `TakaroConan.SOURCE_EVIDENCE.md` SHA-256 values match each other.
- [ ] Installed server/client paks match the SHA-256 and size recorded in `TakaroConan.artifact-manifest.json`.
- [ ] Installed server/client build reports match the installed pak and `TakaroConan.artifact-manifest.json`.
- [ ] Installed server/client source evidence files match `TakaroConan.artifact-manifest.json`.
- [ ] Installed artifact manifests still assert `compiledInDevKit=true`, `cookedInDevKit=true`, and `containsPippiAssets=false`.
- [ ] Server `modlist.txt` contains exactly `*TakaroConan.pak`.
- [ ] Client `modlist.txt` contains exactly `*TakaroConan.pak`.
- [ ] `Pippi.pak` is not in either active modlist.
- [ ] Stop the host Pippi/chat poller process.
- [ ] Restart the Conan dedicated server.
- [ ] Restart/relog the Conan client so it loads the same modlist after the apply wrapper proves it was not running during client modlist mutation.
- [ ] Reconnect to `192.168.129.13:7777`.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-ready-takaro-inputs.sh --post-reconnect-check` runs after reconnect and reaches the strict final checkpoint.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/check-takaro-mod-install.sh` passes.

## Live Mod Gate

- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/check-sidecar-auth.sh` passes.
- [ ] `node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/probe-registration-sources.mjs` finds a valid active-domain registration token candidate, or `check-sidecar-auth.sh` already proves the sidecar is identified.
- [ ] If a new registration token is needed, `node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/apply-registration-token.mjs --apply --restart --kill-existing` validates it with a temporary sidecar on an isolated health port before updating runtime config, replacing the running sidecar, and proving the restarted real sidecar identifies through `/health`.
- [ ] `curl http://127.0.0.1:3010/health` returns `ok=true`, `takaroIdentified=true`, and `modBridge.connected=true`.
- [ ] `.runtime/conan-bridge/TakaroConfig.txt` contains a currently valid Takaro server registration token or other connector credential accepted by `wss://connect.takaro.io/`.
- [ ] `.runtime/conan-bridge/TakaroConfig.txt` has `requireModSourceAttribution=true`, and `/health.modBridge.sourceAttributionRequired=true` plus `/health.modBridge.gameEventValidationEnabled=true`.
- [ ] `modBridge.lastPollSource` contains `TakaroConan`.
- [ ] After outbound chat, `modBridge.lastResultSource` contains `TakaroConan` and `modBridge.lastResultAt` is after the validation run started.
- [ ] After inbound chat, `modBridge.lastEventSource` contains `TakaroConan`, `modBridge.lastEventType="chat-message"`, and `modBridge.lastEventAt` is after the validation run started.
- [ ] `modBridge.recentResults` contains the current server-wide and targeted markers with `action="sendMessage"`, `source` containing `TakaroConan`, and `resultSuccess=true`.
- [ ] `modBridge.recentEvents` contains the current inbound marker with `type="chat-message"`, `source` containing `TakaroConan`, and stable Steam/platform identity.
- [ ] Server logs show `TakaroConan` loaded and polling.
- [ ] Client logs show `TakaroConan` loaded.
- [ ] MCP `gameserverGetPlayers` returns the connected test player after reconnect.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-takaro-mod-live.sh` passes with exact current-marker player-visible, inbound-chat, and module-command confirmations.
- [ ] The live validator writes a JSON receipt under `.runtime/conan-live-receipts/` or the path set by `TAKARO_CONAN_LIVE_RECEIPT`, and that receipt records `status="passed"`, current markers, `gameServerId`, player identity, sanitized health summaries, exact TakaroConan result/event proof entries for the current markers, a fresh module command event after validation start, and no secret values.
- [ ] `bash /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-live-receipt.sh --receipt /path/to/live-receipt.json` passes for the final receipt.
- [ ] After the final receipt and QA ledger prove the replacement,
  `node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/mark-final-goal-validated.mjs --receipt /path/to/live-receipt.json --final-audit-log /path/to/final-audit.log --qa-ledger-section "YYYY-MM-DD final validation"` updates `capabilities.json`.
- [ ] `capabilities.json` has `finalTakaroConanGoal.status="validated"`.
- [ ] `capabilities.json` has
  `finalTakaroConanGoal.evidence.validatedAt`, `liveReceipt`,
  `finalAuditLog`, `qaLedgerSection`, and `postReconnectCommand`.
- [ ] The `finalTakaroConanGoal.evidence.liveReceipt` path exists and points to
  a passed final receipt that satisfies `validate-live-receipt.sh --receipt`,
  including TakaroConan source attribution, player-visible chat, inbound chat,
  stable player identity, Pippi/RCON renderer absence, and fresh module-command
  event proof from the same validation run.
- [ ] `node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-final-goal-status.mjs --require-validated` passes.
- [ ] Final live validation is run with an online player; no no-player bypass is accepted.
- [ ] The connected player confirms the server-wide test message appears in normal chat.
- [ ] The connected player confirms the targeted test message appears only for the target player when a second client is available.
- [ ] A player sends the validator's current inbound marker and Takaro `eventSearch` returns that same `chat-message` with stable Steam/platform identity.
- [ ] A player sends one configured Takaro module command during the same live validation run, `TAKARO_CONAN_MODULE_COMMAND_SENT` is set to the validator's exact current module-command marker only after that command is sent, and Takaro `eventSearch` returns a fresh `command-executed` or `command-execution-denied` event after validation start.
- [ ] The live validator does not find the test message in the Pippi/RCON renderer log path.

## Connector Coverage Gate

- [ ] `npm test` passes in `conan-exiles`.
- [ ] `npm run build` passes in `conan-exiles`.
- [ ] `node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-api-goal-matrix.mjs` passes.
- [ ] `node /home/hendrik/gamingconnectors/context/games/conan-exiles/scripts/validate-module-automation.mjs` passes.
- [ ] Non-destructive MCP checks pass: reachability, players, bans, map info fallback, map tile unsupported, execute `help`.
- [ ] Destructive checks are only repeated on a disposable server or with explicit approval: give item, teleport, kick, ban, unban, shutdown.
- [ ] Installed Takaro module evidence is captured for command and cron behavior; final goal audit requires recent command-module execution proof.

## Current State

As of the latest local validation, this checklist is not complete:

- No local Conan DevKit/cook toolchain was found.
- No `TakaroConan.pak` was found.
- Server and client modlists still point to `*Pippi.pak`.
- The host Pippi/chat poller is still running.
- The Conan client is currently running and must fully exit before final modlist replacement/rejoin validation.
- The sidecar `/health` endpoint is reachable, but `ok=false`, `takaroIdentified=false`, `gameServerId` is missing, and Takaro reports the configured registration token is invalid.
- The current MCP account can see the active domain, but domain token/read/resolve routes return 401, read-only game-server routes expose `identityToken` but no `registrationToken`, and no matching local CSV token export was found.
- Server and client logs do not mention `TakaroConan`.
- `/health.modBridge.lastPollSource` is currently `node`, which proves only the old host poller path, not `TakaroConan`.
- Installed Takaro module inventory and cron/server-message evidence are present, but final audit still needs a fresh player-triggered command execution event.
