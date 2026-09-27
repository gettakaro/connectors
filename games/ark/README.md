# Takaro ARK: Survival Evolved Connector

This server-side native Linux connector targets ARK: Survival Evolved. Players do not install anything. It is pinned to Steam app 376030, public build 21241282, depot 376031 manifest 6366771435093287465, and `ShooterGameServer` SHA-256 `7e7ded49e0c658e74199801d79630bd33da407d3e468e039837bf61a9de7c520`. The native library checks the executable again at launch. A different executable is refused. The target remains **candidate** until the remaining native actions, gameplay events, and recovery behavior are verified.

## Install

### 1. Build and deploy the exact target

These candidate instructions require a checkout of [the connectors repository](https://github.com/gettakaro/connectors), Docker, and an ARK Linux server at the pinned build. Run the commands from the checkout root. They use the catalog and produce exact-target artifacts:

```sh
maintenance/bin/takaro-maint catalog validate > /tmp/ark-catalog-validation.json
maintenance/bin/takaro-maint targets resolve --game ark --target linux-21241282 --format json > /tmp/ark-target.json
maintenance/bin/takaro-maint install --game ark --target linux-21241282 --dest /srv/ark --dry-run
maintenance/bin/takaro-maint install --game ark --target linux-21241282 --dest /srv/ark --reuse-world
maintenance/bin/takaro-maint build --game ark --target linux-21241282 --version 0.1.0 --out /tmp/ark-artifacts
maintenance/bin/takaro-maint deploy --game ark --target linux-21241282 --dest /srv/ark --from /tmp/ark-artifacts/build-manifest.json
```

`build` uses the target's immutable Node Bookworm toolchain image and runs the native and sidecar tests. The deployment writes only `/srv/ark/TakaroArk/TakaroArkNative` and `/srv/ark/TakaroArk/TakaroArkSidecar`, atomically replacing each owned folder. It preserves `ShooterGame/Saved`, `.takaro`, and the other component folder. The `takaro-target.json` and `uninstall-manifest.json` shipped in each zip record its exact ownership and target. Keep the build manifest with the artifacts; do not substitute an older native library into a newer zip.

### 2. Configure and start

Set the same long random `ARK_NATIVE_TOKEN` for the game and sidecar. Start the game through the shipped `/srv/ark/TakaroArk/TakaroArkNative/launch.sh /srv/ark 'TheIsland?listen?SessionName=Takaro-ARK?Port=7787?QueryPort=27025' -server -log -NoBattlEye`. The launcher checks the exact executable hash and sets `LD_PRELOAD` only for `ShooterGameServer`. Do not preload SteamCMD. When containerized, run the game with Docker `--init` or Compose `init: true`, so the game is not namespace PID 1. ARK's normal acknowledged shutdown saves the world and intentionally exits with signal 6 (status 134); a PID 1 process can instead hit glibc's abort fallback and exit 139. The sidecar needs `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_WS_URL`, `ARK_NATIVE_TOKEN`, `ARK_NATIVE_URL=http://127.0.0.1:18891`, and a persistent `TAKARO_CURSOR_FILE`; run it in the server's network namespace so the native bearer API stays on loopback. The shipped `TakaroArkSidecar/Dockerfile` builds from the compiled release files. `dev-servers/compose/ark.yml` supplies a concrete two-container rig.

### 3. Check the connection

For an isolated verification run, use `maintenance/bin/takaro-maint verify --help` and the ARK-specific checks in `maintenance/src/takaro_maint/games/ark/verify.py`. A bare run cannot prove PC chat, player location/inventory, or unsupported actions without real clients; those checks fail openly rather than inventing success. The `--ark-readonly-base` mode passed the pinned v23 isolated protocol, including two acknowledged saves and status-134 exits plus a distinct-boot reload that read the unchanged owned world save. It keeps an owned writable `ShooterGame/Binaries/Linux/BanList.txt` across boots. That protocol run does not cover real PC gameplay. Do not run the verifier against an active production server; it manages its own container and shutdown.

Preserve the game's writable `BanList.txt` separately from the world save. Keep the sidecar's `ban-metadata.json` beside its persistent event cursor; it records reasons and expiry dates that ARK does not store. See [ban metadata and expiry](ban-metadata.md) for migration and timed-ban behavior, and [Generic failure handling](generic-failure-contract.md) for Takaro's bounded timeout on rejected actions.

### 4. Uninstall

To uninstall, stop the sidecar and game, then remove only the two paths named by their `uninstall-manifest.json` records: `/srv/ark/TakaroArk/TakaroArkNative` and `/srv/ark/TakaroArk/TakaroArkSidecar`. Keep `/srv/ark/ShooterGame/Saved` and `/srv/ark/.takaro`; the Steam server files and world are outside connector ownership. The launcher need not remain after uninstall, and starting the ordinary `ShooterGameServer` without it leaves the server unmodified.

## What works, what doesn't

As of 2026-09-24, this remains a candidate for ARK public build **21241282**. The verified v23 artifacts use source revision `e41d7b18da7f1587db30b2c3c8d2ba3d28e4ad76`, native ZIP SHA-256 `de311e194272bc9bf1f0160f3d12618e5947406fd7f1bb023b975d02da27db28`, native library SHA-256 `79f537a4601a549906f637016b9795e8e675756c01277587acd0c769cdb8f6dd`, and sidecar ZIP SHA-256 `021cf966130c417433258beb7cbd9f7ce934bc83747572173ff5bf7a6d6c941f`. Direct and maintenance builds produced identical ZIPs. The [target issue #289](https://github.com/gettakaro/connectors/issues/289) and [draft PR #291](https://github.com/gettakaro/connectors/pull/291) track remaining verification. Earlier gameplay evidence remains historical until verified against the current artifacts.

| Capability | Status | Evidence and limit |
| --- | --- | --- |
| Exact executable guard and deployed identity | ✅ Observed on v23 | The shipped native library and sidecar booted against the pinned server; isolated wrong-target tests reject a different executable. |
| Native health and sidecar connection | ✅ Observed on v23 | Authenticated native health and sidecar identification reported the same live boot ID and ready ban reconciliation. |
| Ban and unban | ✅ Verified with a real client on v23 | A reasoned Takaro ban removed the online player, wrote the native ban file, and blocked a Favorites rejoin. Unban cleared native and Takaro records; the same player returned to the preserved world. |
| Timed expiry and ban-file persistence | ⚠️ Verified in isolation on v23 | A three-boot test retained the ban, reason, and expiry; real wall-clock expiry removed it. The test also covered permanent ban and manual unban; a final boot confirmed removal. Live-client persistence acceptance remains pending. Takaro may retain expired managed database rows; see the metadata documentation. |
| Kick, chat, messaging, and outage recovery | ⚠️ Historical live proof | Earlier builds proved these paths, including the repaired delayed-event recovery. Their full current-artifact acceptance remains pending. |
| Other native actions, gameplay events, modules, and recovery | ❌ Acceptance incomplete | The isolated v23 protocol covers catalogs, roster, handled console execution, and acknowledged save/reload shutdowns. Remaining real-client obligations are tracked in the issue. `listLocations` has no available Takaro SDK/MCP operation despite native support. |
