# Valheim Dynamic Server Chat Sender Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Propagate each Takaro `sendMessage.opts.senderNameOverride` into authenticated normal Valheim chat, using `Takaro` only when the override is missing or blank.

**Architecture:** Extend the existing adapter method boundary with one optional sender argument. Parse and bound the nested Takaro option in the dispatcher, normalize the fallback in the real adapter, and pass the final sender through the existing `server-chat` companion envelope. No wire-schema change is needed because the payload already contains `Sender`.

**Tech Stack:** C#/.NET 8 tests, .NET Framework 4.7.2 BepInEx plugins, MSTest, Valheim routed RPC, Bash release packaging.

---

### Task 1: Propagate Takaro's sender override through the dispatcher

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Core/Adapter.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/RequestDispatcherTests.cs`
- Modify: adapter fakes implementing `IValheimTakaroAdapter` under `valheim/tests/Takaro.Valheim.Core.Tests`

**Step 1: Write failing dispatcher tests**

Add tests proving:

```csharp
await dispatcher.DispatchAsync(new TakaroRequest(
    "sender",
    "sendMessage",
    JsonDocument.Parse(
        """{"message":"Hello","opts":{"senderNameOverride":"  con  "}}""")
        .RootElement));

CollectionAssert.AreEqual(
    new[] { "message:Hello:<global>:con" },
    adapter.Calls);
```

Also prove missing and whitespace-only overrides arrive as `null`, and a sender longer than `CompanionProtocol.MaximumCodeCharacters` returns `invalid_args` without calling the adapter.

**Step 2: Run the focused tests and verify RED**

Run:

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter RequestDispatcherTests -v minimal
```

Expected: FAIL because `SendMessageAsync` has no sender override parameter and the dispatcher drops the option.

**Step 3: Implement strict optional parsing and propagation**

Change the interface to:

```csharp
Task<TakaroActionResult> SendMessageAsync(
    string message,
    string? recipientIdentifier,
    string? senderNameOverride,
    CancellationToken cancellationToken = default);
```

Dispatch with:

```csharp
adapter.SendMessageAsync(
    RequiredString(request.Args, "message"),
    OptionalRecipientIdentifier(request.Args),
    OptionalSenderNameOverride(request.Args),
    cancellationToken)
```

`OptionalSenderNameOverride` must read only `opts.senderNameOverride`, trim it, return `null` for missing/blank values, reject non-string values, and reject values longer than `CompanionProtocol.MaximumCodeCharacters` with `ArgumentException`.

Update every interface implementation and test fake mechanically, preserving existing behavior.

**Step 4: Run the focused tests and verify GREEN**

Run the Task 1 test command again.

Expected: all `RequestDispatcherTests` pass.

**Step 5: Commit**

```bash
git add valheim/src/Takaro.Valheim.Core/Adapter.cs \
  valheim/tests/Takaro.Valheim.Core.Tests
git commit -m "feat(valheim): propagate server chat sender override"
```

### Task 2: Render the dynamic sender through the companion bridge

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs`
- Modify: `valheim/src/Takaro.Valheim.Plugin/CompanionServerBridge.cs`
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionServerBridgeContractTests.cs`

**Step 1: Write failing source-contract tests**

Require the adapter to normalize once:

```csharp
var sender = string.IsNullOrWhiteSpace(senderNameOverride)
    ? "Takaro"
    : senderNameOverride.Trim();
```

Require its companion delegate to accept `(peer, sender, message)`. Require `CompanionServerBridge.TrySendServerChat` to accept `sender` and construct:

```csharp
new CompanionServerChatMessage(sender, message)
```

Reject a hard-coded `new CompanionServerChatMessage("Takaro", message)` in the bridge.

**Step 2: Run the focused tests and verify RED**

Run:

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter "PluginScaffoldContractTests|CompanionServerBridgeContractTests" -v minimal
```

Expected: FAIL because the bridge currently hard-codes `Takaro`.

**Step 3: Implement the dynamic route**

- Update `ValheimServerAdapter.SendMessageAsync` and its scaffold implementation to the new interface.
- Normalize the fallback once at the top of the real method.
- Change `sendCompanionChat` to `Func<ZNetPeer, string, string, bool>`.
- Pass the normalized sender for direct and global delivery.
- Change `TrySendServerChat` to accept the sender and create the existing bounded payload with it.
- Update the plugin lambda to forward all three values.

**Step 4: Run focused tests and real builds**

Run the Task 2 test command, then:

```bash
dotnet build valheim/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  -f net472 -p:EnableValheimPluginBuild=true \
  -p:BepInExReferencePath=/home/hendrik/valheim-dedicated-server/BepInEx/core \
  -p:ValheimReferencePath=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed \
  -v minimal
```

Expected: tests and real plugin build pass with zero warnings/errors.

**Step 5: Commit**

```bash
git add valheim/src/Takaro.Valheim.Plugin \
  valheim/tests/Takaro.Valheim.Core.Tests
git commit -m "feat(valheim): render Takaro sender overrides"
```

### Task 3: Document, package, and live-prove both paths

**Files:**
- Modify: `valheim/README.md`
- Modify: `valheim/COMPANION.md`
- Modify: `valheim/capabilities.json`
- Create after proof: `valheim/qa/2026-07-14-server-chat-validation.md`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CapabilityRegistryTests.cs`

**Step 1: Add failing documentation assertions**

Require the docs to state that `opts.senderNameOverride` is dynamic per message and missing/blank values display as `Takaro`.

**Step 2: Run the documentation tests and verify RED**

Run:

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter CapabilityRegistryTests -v minimal
```

Expected: FAIL until the behavior is documented.

**Step 3: Update docs and run complete verification**

Update the docs without claiming live support yet. Then run:

```bash
dotnet test valheim/Takaro.Valheim.sln -v minimal
git diff --check
```

Expected: the complete suite passes and the tree has no whitespace errors.

**Step 4: Build and install matching 2.0.1 candidate archives**

Use `valheim/scripts/build-release.sh` with the known server references, validate both ZIPs, stop both roles, replace both complete plugin folders, clear `BepInEx/cache/chainloader_typeloader.dat`, and restart.

**Step 5: Run exact live proof**

With the player connected and the negotiated companion active:

1. call Takaro `gameserverSendMessage` with `opts.senderNameOverride: "con"` and a unique marker;
2. confirm the line appears in normal chat as `con`, never as a HUD overlay;
3. call it again without `opts.senderNameOverride` using a second unique marker;
4. confirm the sender is `Takaro` in normal chat;
5. retain MCP responses, server/client logs, artifact hashes, and the user's visual confirmation in the QA ledger.

**Step 6: Promote support status only after proof and commit**

After both live checks pass, change `sendMessage` back to `live-supported`, link the new QA ledger, rerun the complete suite, and commit:

```bash
git add valheim/README.md valheim/COMPANION.md valheim/capabilities.json \
  valheim/qa/2026-07-14-server-chat-validation.md \
  valheim/tests/Takaro.Valheim.Core.Tests/CapabilityRegistryTests.cs
git commit -m "docs(valheim): record live chat sender proof"
```
