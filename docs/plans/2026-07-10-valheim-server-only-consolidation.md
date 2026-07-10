# Valheim Server-Only Consolidation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the divergent Valheim follow-up PRs with one dedicated-server-only connector change that is honest about unsupported client-owned state and packages reliably in CI.

**Architecture:** The BepInEx plugin must exit in non-dedicated processes, connect to Takaro only from the dedicated server, and implement actions using server-owned Valheim APIs. Remove all Takaro client snapshots and custom client action/command RPCs; keep explicit unavailable responses for client-owned state and classify every action/event in a machine-readable capability registry.

**Tech Stack:** C#/.NET 8 test projects, `net472` BepInEx plugin build, Harmony, Valheim dedicated-server assemblies, Bash packaging scripts, GitHub Actions.

---

### Task 1: Lock the server-only boundary with failing contract tests

**Files:**
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`

**Step 1: Replace client-positive scaffold assertions with server-only assertions**

Add focused tests equivalent to:

```csharp
[TestMethod]
public void PluginDoesNotStartOnClientProcesses()
{
    var source = ReadPluginSource("ValheimTakaroPlugin.cs");
    StringAssert.Contains(source, "if (!IsDedicatedServerProcess())");
    StringAssert.Contains(source, "only runs on dedicated Valheim servers");
    Assert.IsTrue(source.IndexOf("if (!IsDedicatedServerProcess())", StringComparison.Ordinal)
        < source.IndexOf("harmony = new Harmony", StringComparison.Ordinal));
    Assert.IsFalse(source.Contains("client bridge started", StringComparison.OrdinalIgnoreCase));
}

[TestMethod]
public void PluginBridgeDoesNotDeclareClientSideRpcContracts()
{
    var source = ReadPluginSource("ValheimChatEventBridge.cs");
    foreach (var marker in new[] {
        "TakaroClientChatMessage", "TakaroClientInventorySnapshot",
        "TakaroClientLocationSnapshot", "TakaroClientChatCommand",
        "TakaroGiveItem", "TakaroTeleportPlayer", "TakaroPlayerDeath",
        "TakaroEntityKilled", "Player.m_localPlayer"
    })
    {
        Assert.IsFalse(source.Contains(marker, StringComparison.Ordinal), marker);
    }
}

[TestMethod]
public void PluginAdapterDoesNotRouteActionsThroughCustomClientRpc()
{
    var source = ReadPluginSource("ValheimServerAdapter.cs");
    Assert.IsFalse(source.Contains("TakaroGiveItem", StringComparison.Ordinal));
    Assert.IsFalse(source.Contains("TakaroTeleportPlayer", StringComparison.Ordinal));
    Assert.IsFalse(source.Contains("TakaroServerMessage", StringComparison.Ordinal));
    Assert.IsFalse(source.Contains("TryGetLocationSnapshot", StringComparison.Ordinal));
    Assert.IsFalse(source.Contains("TryGetInventorySnapshot", StringComparison.Ordinal));
}

[TestMethod]
public void PluginAdapterReturnsExplicitUnavailableErrorsForClientOwnedState()
{
    var source = ReadPluginSource("ValheimServerAdapter.cs");
    var location = SliceMethod(source, "public Task<TakaroActionResult> GetPlayerLocationAsync", "public Task<TakaroActionResult> GetPlayerInventoryAsync");
    var inventory = SliceMethod(source, "public Task<TakaroActionResult> GetPlayerInventoryAsync", "public Task<TakaroActionResult> GiveItemAsync");
    StringAssert.Contains(location, "player_position_unavailable");
    Assert.IsFalse(location.Contains("new TakaroPosition(0, 0, 0", StringComparison.Ordinal));
    StringAssert.Contains(inventory, "player_component_unavailable");
    Assert.IsFalse(inventory.Contains("Array.Empty<object>()", StringComparison.Ordinal));
}
```

Retain the existing moderation, console allowlist, and location-list assertions.

**Step 2: Run the focused test class and verify RED**

Run:

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter PluginScaffoldContractTests -v minimal
```

Expected: FAIL because `main` still starts a client bridge, declares custom client RPCs, and returns fake origin/empty inventory success.

**Step 3: Do not commit yet**

Keep the failing tests in the working tree for Tasks 2 and 3.

### Task 2: Make plugin startup and event capture dedicated-server-only

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs`
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimChatEventBridge.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`

**Step 1: Guard the plugin entrypoint before Harmony or runner startup**

In `Awake()`, the first runtime decision must be:

```csharp
if (!IsDedicatedServerProcess())
{
    Logger.LogWarning("Takaro Valheim only runs on dedicated Valheim servers; client process detected, plugin disabled.");
    enabled = false;
    return;
}
```

Only after this guard may the code create Harmony patches, read Takaro credentials, create `TakaroWebSocketRunner`, or register bridge state.

**Step 2: Remove every client-owned bridge path**

Delete client snapshot caches, client update senders, client death/entity forwarding, custom client RPC registration/handlers, and client-side Harmony patches. The remaining bridge may:

- observe server-routed `ChatMessage`/`Say` packets defensively;
- emit only connector logs and server-snapshot `player-connected`/`player-disconnected` events;
- reject routed packet identity/death payloads as diagnostic-only;
- emit connector log events.

Do not emit or claim ordinary inbound chat, player-death, or entity-killed. No death/entity Harmony emitter remains in the final server-only connector.

Use PR #78 only as a behavioral reference:

```bash
git show origin/fix/remove-valheim-client-mod-code:valheim/src/Takaro.Valheim.Plugin/ValheimChatEventBridge.cs
```

Do not cherry-pick its commits.

**Step 3: Run the focused tests**

Run the Task 1 command. Expected: startup/bridge assertions PASS; adapter assertions may still FAIL.

**Step 4: Commit the server-only lifecycle**

```bash
git add valheim/src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs \
  valheim/src/Takaro.Valheim.Plugin/ValheimChatEventBridge.cs \
  valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs
git commit -m "fix(valheim): enforce dedicated-server-only runtime"
```

### Task 3: Replace client actions and fake player state with server-owned behavior

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`

**Step 1: Add failing assertions for server-owned give and teleport**

Add:

```csharp
[TestMethod]
public void PluginAdapterUsesServerOwnedGiveAndTeleportPaths()
{
    var source = ReadPluginSource("ValheimServerAdapter.cs");
    var give = SliceMethod(source, "public Task<TakaroActionResult> GiveItemAsync", "public Task<TakaroActionResult> SendMessageAsync");
    var teleport = SliceMethod(source, "public Task<TakaroActionResult> TeleportPlayerAsync", "public Task<TakaroActionResult> KickPlayerAsync");
    StringAssert.Contains(give, "ItemDrop");
    StringAssert.Contains(give, "Instantiate");
    StringAssert.Contains(teleport, "RPC_TeleportTo");
    Assert.IsFalse(give.Contains("TakaroGiveItem", StringComparison.Ordinal));
    Assert.IsFalse(teleport.Contains("TakaroTeleportPlayer", StringComparison.Ordinal));
}
```

Run the focused test and confirm this new test fails for the expected missing server-owned paths.

**Step 2: Implement honest location and inventory behavior**

- Use only `ZNetPeer.m_refPos`, public-position data, or a fresh player-keyed last-known observation captured from those server-owned sources.
- Retain last-known positions for 30 seconds across disconnect, expire them, and clear them when Valheim replaces its network/world instance. Never cache an origin placeholder.
- Gate `player-connected` tracking until a real server-owned position exists so lifecycle enrichment does not race character readiness.
- When no current/fresh location exists, emit required numeric coordinates plus `payload.error`; current Takaro source commit `0c63cf1c` validates the position DTO and rejects that payload before returning it.
- Treat remote inventory as permanently unsupported on a dedicated server. Return `player_component_unavailable` internally and send no WebSocket response, allowing Takaro's bounded pending-request timeout rather than fabricating `[]`.
- Rate-limit the explicit no-response compatibility log. Root-level `success`/`errorCode` fields are not a supported failure contract.

**Step 3: Implement server-owned `giveItem`**

- Resolve prefab by code and display/token name.
- Validate positive amount and quality.
- Resolve the online player's server-known position.
- Spawn `ItemDrop` world objects near the player, splitting stacks when required.
- Return a structured error when the prefab or server-owned position is unavailable.
- Send a routed confirmation message without a custom Takaro client RPC.

**Step 4: Implement server-owned `teleportPlayer`**

- Resolve the peer and character ZDO.
- Route Valheim's built-in `RPC_TeleportTo` to the character owner.
- Return a structured unavailable error if no character ZDO is known.

Use the relevant methods on PR #79 only as behavioral reference; do not port `InitializeClient`, `TakaroClientChatCommand`, or any snapshot/RPC code:

```bash
git show origin/feature/valheim-client-command-bridge:valheim/src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs
```

**Step 5: Run focused and full tests**

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter PluginScaffoldContractTests -v minimal
dotnet test valheim/Takaro.Valheim.sln --no-restore -v minimal
```

Expected: all tests PASS.

**Step 6: Commit**

```bash
git add valheim/src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs \
  valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs
git commit -m "fix(valheim): use server-owned player actions"
```

### Task 4: Remove Jotunn and harden CI dependency setup

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj`
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs`
- Modify: `valheim/scripts/setup-environment.sh`
- Modify: `valheim/scripts/build-release.sh`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`

**Step 1: Add a failing dependency/package contract test**

Add a test that reads the project, entrypoint, setup script, and release script and asserts that none contains `Jotunn`, `JOTUNN_REFERENCE_PATH`, or `BepInDependency`, while setup contains bounded retry markers and both `linux` and `windows` platform fallback.

Run the focused test and confirm RED because current `main` still requires Jotunn and has a single SteamCMD attempt.

**Step 2: Remove Jotunn from build/runtime**

- Delete the Jotunn reference from the plugin project.
- Delete the BepIn dependency attribute.
- Remove Jotunn download/reference handling from both scripts and packaged README.
- Preserve Harmony through BepInEx.

**Step 3: Add bounded setup retries**

Adapt PR #72's setup behavior:

- `VALHEIM_STEAM_PLATFORMS` defaults to `linux windows`;
- maximum three attempts per platform;
- verify `valheim_server_Data/Managed` after every attempt;
- clear Steam app cache between retries;
- fall back to the Windows depot only for compile references;
- use `curl --retry 5 --retry-delay 2 --retry-all-errors` for BepInEx downloads;
- fail non-zero if managed assemblies never appear.

**Step 4: Run static and focused tests**

```bash
bash -n valheim/scripts/setup-environment.sh valheim/scripts/build-release.sh
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter PluginScaffoldContractTests -v minimal
```

Expected: PASS.

**Step 5: Commit**

```bash
git add valheim/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  valheim/src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs \
  valheim/scripts/setup-environment.sh valheim/scripts/build-release.sh \
  valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs
git commit -m "fix(valheim): harden server-only packaging"
```

### Task 5: Add an exhaustive capability registry and honest documentation

**Files:**
- Create: `valheim/capabilities.json`
- Create: `valheim/tests/Takaro.Valheim.Core.Tests/CapabilityRegistryTests.cs`
- Modify: `valheim/README.md`

**Step 1: Write the failing registry completeness test**

Create a test that parses `valheim/capabilities.json`, asserts every status is exactly `live-supported`, `schema-fallback`, or `unsupported`, and requires these actions/events:

```csharp
string[] actions = [
    "testReachability", "getPlayers", "getPlayer", "getPlayerLocation",
    "getPlayerInventory", "giveItem", "sendMessage", "executeConsoleCommand",
    "listItems", "listEntities", "listLocations", "teleportPlayer",
    "kickPlayer", "banPlayer", "unbanPlayer", "listBans", "shutdown"
];

string[] events = [
    "log", "player-connected", "player-disconnected",
    "chat-message", "player-death", "entity-killed"
];
```

Run:

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter CapabilityRegistryTests -v minimal
```

Expected: FAIL because the registry does not exist.

**Step 2: Create the registry**

Use an object with `architecture`, `actions`, `events`, and `notes`. Classify only proven server-owned paths as `live-supported`. Required conservative classifications include:

- `getPlayerInventory`: `unsupported`; the dedicated server has no remote inventory path, and the wire deliberately emits no response rather than a false empty array;
- `listLocations`: `schema-fallback` when the official raw Generic Connector action/schema is live-proven but the pinned standard Takaro route remains `NotImplemented`;
- `chat-message`: `unsupported` until vanilla-client input is observed at the dedicated server and in Takaro;
- `player-death` and `entity-killed`: `unsupported` with no active emitter;
- `player-connected` and `player-disconnected`: `live-supported` after turn-3 Takaro searches persisted two complete cycles;
- no `implemented`, `partial`, or `stub` values.

If current live proof cannot establish `getPlayer`, classify it conservatively and explain the limitation rather than inventing a proof.

**Step 3: Synchronize the README**

- State that installation is dedicated-server-only and no client mod is supported.
- Remove Jotunn instructions.
- Add a complete action/event support matrix matching the JSON registry.
- Explain world-drop semantics for `giveItem`, built-in teleport semantics, the current-source `payload.error` position behavior, inventory no-response timeout limitation, and unsupported inbound chat/death/entity events.
- Link issue #69 for inbound chat.
- Separate historical live proof from current verification.

**Step 4: Run registry and full tests**

Expected: PASS.

**Step 5: Commit**

```bash
git add valheim/capabilities.json valheim/README.md \
  valheim/tests/Takaro.Valheim.Core.Tests/CapabilityRegistryTests.cs
git commit -m "docs(valheim): publish server-only support matrix"
```

### Task 6: Verify real build and release artifact

**Files:**
- Modify only if verification exposes a defect in the scoped Valheim files.

**Step 1: Run all local gates sequentially**

```bash
dotnet test valheim/Takaro.Valheim.sln --no-restore -v minimal
bash -n valheim/scripts/setup-environment.sh valheim/scripts/build-release.sh
git diff --check origin/main...HEAD
bash scripts/check-commit-title.sh "fix(valheim): consolidate server-only connector"
```

Expected: PASS.

**Step 2: Build the real plugin against local dedicated-server references**

```bash
dotnet build valheim/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -f net472 \
  -p:EnableValheimPluginBuild=true \
  -p:BepInExReferencePath=/home/hendrik/valheim-dedicated-server/BepInEx/core \
  -p:ValheimReferencePath=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed \
  -v minimal
```

Expected: build succeeds with zero errors and no Jotunn reference.

**Step 3: Build and inspect a release zip**

```bash
rm -rf /tmp/valheim-server-only-release
VALHEIM_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed \
BEPINEX_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/BepInEx/core \
bash valheim/scripts/build-release.sh 0.0.0-dev /tmp/valheim-server-only-release
unzip -l /tmp/valheim-server-only-release/takaro-valheim-plugin.zip
```

Expected: zip contains the real plugin, core library, required NuGet runtime DLLs, and README; it contains no tests, Jotunn DLL, client guide, or client-only artifact.

**Step 4: Inspect packaged strings**

Confirm `TakaroValheim.dll` contains `com.takaro.valheim` and does not contain the banned client RPC markers from Task 1.

**Step 5: Commit any verification-only corrections using TDD**

Do not commit generated build output.

### Task 7: Run server-boundary and Takaro QA

**Files:**
- Create: `valheim/qa/2026-07-10-server-only-validation.md`
- Modify capability claims only if live evidence changes their status.

**Step 1: Protect the client boundary**

Before starting Valheim, verify no Takaro plugin exists in the Valheim client plugin directory. Do not delete unrelated client mods. Record the server artifact hash and deployed server DLL hash.

**Step 2: Deploy and start the dedicated-server plugin**

Use the local test server without printing registration/identity tokens. Confirm BepInEx loads the current artifact, WebSocket identify completes, and Takaro reports the expected game server identity.

**Step 3: Run non-destructive connector checks**

Use Takaro MCP plus dedicated-server logs for reachability, players/getPlayer, real location plus unavailable-location rejection, inventory timeout without state mutation, outbound message, world-drop `giveItem`, teleport, list items/entities, the official raw Generic Connector `listLocations` action, the separate standard Takaro `listLocations` route, allowlisted console behavior, persisted lifecycle searches, and zero unsupported death/entity emissions. Resolve the Takaro player UUID before `gameserverGiveItem`.

**Step 4: Run installed-module checks**

Record the current installed modules. Exercise harmless hook/command paths that are genuinely supported and run a `serverMessages` cron item, proving Takaro execution plus server/player-visible delivery. Do not claim command ingress through unsupported vanilla chat.

**Step 5: Skip or approval-gate destructive checks**

Do not ban, kick, or shut down a non-disposable server without explicit approval. Record skipped checks honestly.

**Step 6: Write the evidence ledger**

The QA document must include commit/artifact hashes, runtime versions, game server ID, exact safe commands/tools, timestamped log excerpts without secrets, module evidence, skipped destructive checks, and a final PASS/PASS WITH GAPS/FAIL verdict.

**Step 7: Re-run tests and commit evidence/status corrections**

```bash
dotnet test valheim/Takaro.Valheim.sln --no-restore -v minimal
git diff --check origin/main...HEAD
git add valheim/qa/2026-07-10-server-only-validation.md valheim/capabilities.json valheim/README.md
git commit -m "test(valheim): record server-only live validation"
```

Only add `capabilities.json` and `README.md` if live proof actually changes them.

### Task 8: Branch-wide verification and PR handoff

**Files:**
- Modify only in response to verification findings.

**Step 1: Run the required verification pipeline**

Run `/verify --mode=report-only --scope=branch`. The exerciser, custom gates, and independent Codex review must be present and passing as required by the player-coach workflow.

**Step 2: Fix every issue at severity 5 or higher using a new red-green cycle**

Repeat verification until approved.

**Step 3: Create the PR through the create-pr workflow**

Use title:

```text
fix(valheim): consolidate server-only connector
```

The PR body must explain the dedicated-server boundary, superseded PRs #71/#72/#78/#79, issue #69, capability limitations, package retry fix, tests, live evidence, and skipped destructive checks.

**Step 4: Follow GitHub Actions until green**

Investigate and fix failures; do not stop after pushing. When the replacement PR is green, close superseded PRs with a link to it. Keep issue #69 open unless a vanilla-client server-only chat route was truly proven.

**Step 5: Integration gate**

Merge only when branch protection/review permits. After merge, refresh/re-run Valheim release PR #70 and verify its package job before merging the release PR.
