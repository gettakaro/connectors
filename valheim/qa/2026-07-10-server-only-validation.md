# Valheim Server-Only Validation Ledger

- Validation timestamp: `2026-07-10T13:14:30+02:00`
- Artifact source commit: `2cbc6df7b7cf3c4293d3f65b78def1b61d2fca5d`
- Historical Takaro game server ID: `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab`
- Verdict: **PASS WITH GAPS**

The source, real plugin build, and release archive pass. Fresh runtime validation did not run because the dedicated server, game client, and local Takaro MCP were stopped, while an older Takaro plugin was still installed in the client. Starting that state would violate the server-only preflight.

## Safety and Runtime Boundary

Read-only preflight found:

- no `valheim_server.x86_64` process;
- no `valheim.x86_64` process;
- no tmux server session;
- no listener on local MCP port `3000`;
- a Takaro DLL installed on both server and client, with the same older hash;
- no server or client deployment was changed during this validation.

| Artifact | SHA-256 | State |
| --- | --- | --- |
| Release zip | `57c4b04dd5eabea18da2b625c8dae9b14b0d338514eabf19e0aa91b702142110` | Built from this branch on July 10 |
| Plugin DLL inside release zip | `a1e6e8bf42b10b36b36fab6ef7f83fc2d9b348171129412921982a3c0b9fea8e` | Current server-only build |
| Deployed server DLL | `36e1dd1a6dab5132595640398afb9aa34c3baf91f590d6cc6d8c007cf3059343` | Older July 5 deployment |
| Installed client DLL | `36e1dd1a6dab5132595640398afb9aa34c3baf91f590d6cc6d8c007cf3059343` | Boundary blocker; older July 5 deployment |

The current artifact was therefore not deployed or live-tested. The client DLL must be removed or quarantined before a vanilla-client run, but that external installation was outside this implementation worktree and was left untouched.

## Source and Package Gates

Commands run from the isolated connector worktree:

```bash
dotnet test valheim/Takaro.Valheim.sln --no-restore -v minimal
bash -n valheim/scripts/setup-environment.sh valheim/scripts/build-release.sh
git diff --check origin/main...HEAD
bash scripts/check-commit-title.sh "fix(valheim): consolidate server-only connector"
dotnet build valheim/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -f net472 \
  -p:EnableValheimPluginBuild=true \
  -p:BepInExReferencePath=/home/hendrik/valheim-dedicated-server/BepInEx/core \
  -p:ValheimReferencePath=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed \
  -v minimal
VALHEIM_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed \
BEPINEX_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/BepInEx/core \
bash valheim/scripts/build-release.sh 0.0.0-dev /tmp/valheim-server-only-release
unzip -l /tmp/valheim-server-only-release/takaro-valheim-plugin.zip
```

Results:

- `68` tests passed, `0` failed;
- both shell scripts passed `bash -n`;
- branch diff whitespace and the proposed conventional title passed;
- the real `net472` plugin build completed with `0` warnings and `0` errors;
- the zip contains `TakaroValheim.dll`, `Takaro.Valheim.Core.dll`, required NuGet runtime DLLs, and `README.txt`;
- the zip contains no tests, client guide/artifact, or removed optional dependency;
- packaged strings contain `com.takaro.valheim` and none of the banned custom client RPC markers.

## Runtime Versions and Historical Log Boundary

The newest available logs are historical, not evidence from this branch:

- `/home/hendrik/valheim-dedicated-server/valheim-server.out` modified `2026-07-05T14:12:45+02:00`;
- `/home/hendrik/valheim-dedicated-server/BepInEx/LogOutput.log` modified `2026-07-05T14:12:50+02:00`;
- BepInEx `5.4.23.3` / BepInExPack Valheim `5.4.2333`;
- Unity `6000.0.61f1`;
- Valheim dedicated server `l-0.221.12`, network version `36`;
- local .NET SDK `8.0.128`.

Selected historical, secret-free excerpts from the July 5 server log:

```text
124: Takaro Valheim identified as gameServerId=4dadfdf6-18a3-41f1-ae2c-b94200dea9ab.
437: Takaro Valheim player-connected event sent for Hehe (Steam_76561198000735875).
537: Takaro Valheim server chat message routed to 1 peer(s).
542: Takaro Valheim dropped 1x Wood for Hehe (Steam_76561198000735875) at x=72.81431, y=36.09803, z=-2.961958.
548: Takaro Valheim routed base-game teleportPlayer to Hehe (Steam_76561198000735875): x=73, y=36, z=-3.
```

These lines support the historical capability record, but the deployed binary hash differs from the current artifact and a client plugin was installed during that run. They do not count as fresh server-only proof.

## Connector and Module Checks

Current non-destructive MCP actions were not called because no local MCP listener or game runtime was available. Historical evidence records reachability, player listing/location, outbound messaging, world-drop `giveItem`, built-in teleport, items/entities, ban listing, lifecycle events, and direct Generic Connector `listLocations` coverage.

The last recorded Takaro module query on June 21 reported `teleports`, `Waypoints`, and `serverMessages`. That list was not re-queried on July 10. A June 21 archived server log records a `sendMessage` request and routing to one peer at lines `3472-3473`, previously attributed to the `serverMessages` cron run. Current module installation, harmless hook/command execution, cron execution, and player-visible delivery remain unproven for this branch.

## Skipped Checks

- Fresh `kickPlayer`, `banPlayer`, `unbanPlayer`, and `shutdown` were skipped because the server was stopped and no disposable live session was prepared.
- No plugin files were removed from the client or copied to the server.
- No Takaro token, identity token, raw MCP session, or unfiltered config/log content was printed.
- Inbound vanilla-client chat was not attempted and remains unsupported under issue `#69`.
- Server-only `entity-killed` was not re-proven and remains unsupported.
- `getPlayer` final Takaro response shape was not re-proven and remains unsupported.

## Required Fresh Run

Before release readiness can be claimed:

1. Remove or quarantine only the Takaro DLL from the Valheim client and confirm a vanilla-client boundary.
2. Deploy the current release DLL to the dedicated server and verify the deployed hash equals `a1e6e8bf42b10b36b36fab6ef7f83fc2d9b348171129412921982a3c0b9fea8e`.
3. Start the server and MCP, confirm current BepInEx load, WebSocket identify, and the expected game server ID.
4. Run the safe action/event matrix, current installed-module query, harmless hook/command flow, and a `serverMessages` cron delivery with player-visible confirmation.
5. Keep destructive actions approval-gated and record any skips.

Until that run exists, this branch is build/package ready but only **PASS WITH GAPS** for live QA.
