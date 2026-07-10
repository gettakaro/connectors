# Valheim Server-Only Validation Ledger

- Live validation window: `2026-07-10T13:44:08+02:00` to approximately `2026-07-10T13:55:24+02:00`
- Live artifact source commit: `2cbc6df7b7cf3c4293d3f65b78def1b61d2fca5d`
- Takaro game server ID: `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab`
- Verdict: **BUILD/SERVER-CORE PASS; LIVE-PLAYER BLOCKED**

The turn-1 source, real plugin build, release archive, dedicated-server load, Takaro identify, MCP path, and safe server-core requests passed. Vanilla-client automation did not create or join a player, so this ledger claims no player-bound, catalog, lifecycle/death, hook/command, or player-visible proof. The immediate join blocker is UI automation/profile state outside this connector branch, not a connector failure.

The feedback fixes made after this live window have local executable coverage but were not redeployed for another live-player run. The hashes below identify the exact pre-feedback artifact that produced the retained runtime evidence.

## Artifact and Client Boundary

| Artifact | SHA-256 | Result |
| --- | --- | --- |
| Release zip | `57c4b04dd5eabea18da2b625c8dae9b14b0d338514eabf19e0aa91b702142110` | Built and inspected |
| Plugin DLL inside release zip | `a1e6e8bf42b10b36b36fab6ef7f83fc2d9b348171129412921982a3c0b9fea8e` | Server-only turn-1 DLL |
| Deployed server `TakaroValheim.dll` | `a1e6e8bf42b10b36b36fab6ef7f83fc2d9b348171129412921982a3c0b9fea8e` | Exact match with zip DLL |

Before the run, the old client plugin was moved outside the plugin tree to `/home/hendrik/.local/state/takaro-valheim-backups/20260710-server-only-consolidation-predeploy/client/TakaroValheim`. This read-only boundary check then returned no hits:

```bash
find /home/hendrik/.local/share/Steam/steamapps/common/Valheim/BepInEx/plugins \
  -type f -name TakaroValheim.dll
```

The Steam-launched client had no Takaro DLL and produced no BepInEx Takaro load markers. No unrelated client mod was removed.

## Build and Package Gates

Turn 1 ran these implementation gates from the isolated connector worktree:

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

Turn-1 results were `68` tests passed, shell syntax and branch/title gates passed, the real `net472` build completed with zero warnings and errors, and the package contained the plugin, Core library, required NuGet runtime DLLs, and README. It contained no tests, client guide/artifact, removed optional dependency, or banned custom client RPC marker.

## Dedicated Server and Takaro Core

Primary retained server evidence is `/home/hendrik/valheim-dedicated-server/BepInEx/LogOutput.log`. The bounded exercise transcript is `/tmp/codex-valheim-exercise-20260710.log` (mtime approximately `13:55:24+02:00`). Neither is treated as a committed artifact, and no secret-bearing raw log is reproduced here.

The dedicated server loaded `Takaro Valheim 0.1.0`, identified twice as game server `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab`, and logged `Game server connected` at line `214` around `13:44:26+02:00`. The local MCP authenticated and listened on `127.0.0.1:3000`.

Runtime inventory recorded during the validation:

- BepInEx `5.4.23.3` / BepInExPack Valheim `5.4.2333`;
- Unity `6000.0.61f1`;
- Valheim dedicated server `l-0.221.12`, network version `36`;
- local .NET SDK `8.0.128`.

Safe live results:

| Check | Result | Evidence boundary |
| --- | --- | --- |
| `testReachability` | **PASS**: `{connectable:true}` | Connector request/response succeeded in server log |
| `getPlayers` | **PASS**: `[]` | Correct empty server list; not player proof |
| executeConsoleCommand `help` | **PASS** | Request/response succeeded at server-log lines `343-346` |
| `listBans` | **PASS**: `[]` | Request/response succeeded at lines `347-349` |
| installed modules | **PASS** | Current query returned `teleports`, `Waypoints`, and `serverMessages` |

## Module and Cron Evidence

The `serverMessages` cron reached the connector with request ID `74519227-9812-4c0f-b50f-c575a84772cd`. Server-log lines `486-488` record `server message routed to 0 peer(s)` followed by a successful response.

This proves Takaro cron scheduling and delivery to the connector, but no client was connected:

- cron-to-connector plumbing: **PASS**;
- connector `sendMessage` handling: **PASS/PARTIAL** with zero recipients;
- player-visible delivery: **BLOCKED**;
- harmless installed-module hook/command execution with a player: **BLOCKED**.

## Exact Vanilla-Client Blocker

The unmodded Steam-launched client reached the main menu, but the fixed click `(610,598)` did not transition from the main menu to character selection. `Player.log` at approximately `13:49:53+02:00` records a failed load of `/characters/odin.fch.new.fch` followed by `No player data`; the existing profile is `characters/hehe.fch`.

The server consequently observed no handshake, character ZDO, player-connected event, or nonempty getPlayers result. Fixed-coordinate automation never reached a state where connector player behavior could be exercised. This is the exact external blocker; the ledger does not reinterpret a menu click or empty player list as player-bound proof.

## Blocked, Unsupported, and Skipped Checks

Blocked by the absent live player:

- `getPlayer`, `getPlayerLocation`, `giveItem`, and `teleportPlayer`;
- player-visible messaging and visible `serverMessages` cron delivery;
- player lifecycle/death and character-ZDO paths;
- harmless installed-module hook/command execution;
- current `listItems/listEntities/listLocations` catalog paths in the same bounded exercise.

Intentionally unsupported or conservatively unclaimed:

- `getPlayerInventory`, because remote inventory state is client-owned;
- inbound `chat-message`, tracked by issue `#69`;
- routed `player-death`, because packet identity/death state is not trusted server-owned proof;
- `entity-killed`, until the server-owned observer is re-proven.

Fresh `kickPlayer`, `banPlayer`, `unbanPlayer`, and `shutdown` were skipped because they are destructive and had no approval/disposable live-player target. No token, identity token, raw MCP session, or unfiltered secret-bearing config/log content was printed.

## Cleanup and Remaining Gate

Cleanup passed: temporary client, server, MCP, and tmux sessions were stopped, and the verifier changed no connector code or persistent runtime configuration.

A future player-bound run must select the valid `hehe` profile (or otherwise repair the external profile/UI automation), join the current dedicated server, confirm a handshake and nonempty player list, then exercise the blocked safe actions and player-visible module delivery. Destructive actions remain approval-gated.

Final status remains **BUILD/SERVER-CORE PASS; LIVE-PLAYER BLOCKED**.
