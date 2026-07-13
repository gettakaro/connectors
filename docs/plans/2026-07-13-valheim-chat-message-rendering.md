# Valheim Chat Message Rendering Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make Takaro `sendMessage` render exclusively as authenticated normal Valheim chat through the required owned client companion.

**Architecture:** Extend the existing version-1 companion envelope with a negotiated `ServerChat` capability and a server-to-client `server-chat` payload. The server adapter delegates delivery to `CompanionServerBridge`; the client validates the active server/session and renders the accepted payload through `Chat.instance.AddString("Takaro", message, Talker.Type.Normal)` without invoking HUD APIs.

**Tech Stack:** C#/.NET 8 and net472, BepInEx 5, Valheim routed RPC, System.Text.Json, MSTest, Bash release packaging, Takaro Generic Connector/MCP.

---

### Task 1: Extend the companion wire contract

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Companion.Protocol/CompanionProtocol.cs`
- Modify: `valheim/src/Takaro.Valheim.Companion.Protocol/CompanionMessages.cs`
- Modify: `valheim/src/Takaro.Valheim.Companion.Protocol/CompanionEnvelopeCodec.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionProtocolTests.cs`

**Step 1: Write failing protocol tests**

Add assertions that `ServerChat` is capability bit `16`, `ServerChat` is the exact message type `server-chat`, and `CompanionServerChatMessage` contains only `Sender` and `Message`. Add accepted and rejected payload cases for blank, null, unknown, incorrectly cased, and oversized strings.

```csharp
Assert.AreEqual(16, GetEnumValue(capabilityType, "ServerChat"));
Assert.AreEqual("server-chat", GetConstant<string>("CompanionMessageTypes", "ServerChat"));
AssertDeclaredPayloadAccepted(
    CompanionMessageTypes.ServerChat,
    """{"sender":"Takaro","message":"Hello"}""");
```

**Step 2: Run the focused test and verify RED**

Run: `dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj --filter CompanionProtocolTests -v minimal`

Expected: FAIL because the capability, type, and payload do not exist.

**Step 3: Implement the minimal wire contract**

Add:

```csharp
ServerChat = 16
public const string ServerChat = "server-chat";
public sealed record CompanionServerChatMessage(string Sender, string Message);
```

Update every codec known-type, strict-field, type-map, normalization, semantic-validation, and known-capability branch. Bound both strings, using the existing chat character limit.

**Step 4: Run focused tests and verify GREEN**

Run the command from Step 2.

Expected: all `CompanionProtocolTests` pass.

**Step 5: Commit**

```bash
git add valheim/src/Takaro.Valheim.Companion.Protocol valheim/tests/Takaro.Valheim.Core.Tests/CompanionProtocolTests.cs
git commit -m "feat(valheim): define companion server chat protocol"
```

### Task 2: Validate server-to-client chat session state

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Companion/CompanionClientState.cs`
- Modify: `valheim/src/Takaro.Valheim.Core/CompanionSessionRegistry.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionClientStateTests.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionSessionRegistryTests.cs`

**Step 1: Write failing state tests**

Negotiate a session with `ServerChat`, then assert the client accepts sequence `2` from the current nonce/version and returns the decoded payload. Assert it rejects pre-negotiation, absent capability, wrong nonce/version, duplicate/replayed sequence, wrong type, and retired sessions. Assert the server registry reports a current negotiated capability only before heartbeat expiry.

```csharp
Assert.IsTrue(state.TryAcceptServerChat(envelope, out var chat));
Assert.AreEqual("Takaro", chat!.Sender);
Assert.IsFalse(state.TryAcceptServerChat(envelope, out _)); // replay
```

**Step 2: Run focused tests and verify RED**

Run: `dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj --filter "CompanionClientStateTests|CompanionSessionRegistryTests" -v minimal`

Expected: FAIL because server-chat session validation is missing.

**Step 3: Implement minimal state validation**

Add a separate `lastServerSequence` initialized from the accepted hello sequence, reset with the session, and advanced only after a valid `server-chat` payload is decoded. Add a registry helper that returns only a negotiated, unexpired snapshot containing the requested capability.

**Step 4: Run focused tests and verify GREEN**

Run the command from Step 2.

Expected: all selected state tests pass.

**Step 5: Commit**

```bash
git add valheim/src/Takaro.Valheim.Companion/CompanionClientState.cs valheim/src/Takaro.Valheim.Core/CompanionSessionRegistry.cs valheim/tests/Takaro.Valheim.Core.Tests
git commit -m "feat(valheim): validate server chat sessions"
```

### Task 3: Render authenticated companion messages in chat

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Companion/CompanionClientBridge.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionPluginContractTests.cs`

**Step 1: Write failing client contract tests**

Require `CompanionCapability.ServerChat`, a call to `state.TryAcceptServerChat`, sender validation through the current server peer, and this rendering route:

```csharp
Chat.instance.AddString(chat.Sender, chat.Message, Talker.Type.Normal);
Chat.instance.m_hideTimer = 0f;
```

Reject `MessageHud`, `ShowMessage`, and client-side rebroadcast through `Chat.SendText` in the server-chat handler.

**Step 2: Run the focused test and verify RED**

Run: `dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj --filter CompanionPluginContractTests -v minimal`

Expected: FAIL because the client handles only hello envelopes.

**Step 3: Implement minimal rendering**

After active-server validation, branch on `server-chat`; validate it with client state, require `Chat.instance`, add exactly one normal line, reset the hide timer, and log delivery. Keep hello negotiation unchanged.

**Step 4: Build the real companion and run tests**

Run:

```bash
BEPINEX_REFERENCE_PATH=/home/hendrik/.local/share/Steam/steamapps/common/Valheim/BepInEx/core \
VALHEIM_REFERENCE_PATH=/home/hendrik/.local/share/Steam/steamapps/common/Valheim/valheim_Data/Managed \
dotnet build valheim/src/Takaro.Valheim.Companion/Takaro.Valheim.Companion.csproj -f net472 -p:EnableValheimCompanionBuild=true -v minimal
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj --filter CompanionPluginContractTests -v minimal
```

Expected: build and tests pass against the installed current Valheim client references.

**Step 5: Commit**

```bash
git add valheim/src/Takaro.Valheim.Companion/CompanionClientBridge.cs valheim/tests/Takaro.Valheim.Core.Tests/CompanionPluginContractTests.cs
git commit -m "feat(valheim): render server messages in chat"
```

### Task 4: Route adapter messages only through compatible companions

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Plugin/CompanionServerBridge.cs`
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs`
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionServerBridgeContractTests.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`

**Step 1: Write failing server-routing tests**

Require a bridge method that creates `server-chat` envelopes from a negotiated session using a server-owned sequence, sends through `CompanionProtocol.RpcName`, and returns an explicit delivery result. Require `SendMessageAsync` to use that method for direct and global delivery. Assert its source slice contains neither `SendHudMessage`, `MessageHud`, `Message`, nor `ShowMessage`.

**Step 2: Run focused tests and verify RED**

Run: `dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj --filter "CompanionServerBridgeContractTests|PluginScaffoldContractTests" -v minimal`

Expected: FAIL because `sendMessage` still invokes HUD delivery.

**Step 3: Implement companion-only routing**

Track the next server sequence per active peer, starting at `2` after hello. `TrySendServerChat` requires an unexpired negotiated `ServerChat` capability, encodes `CompanionServerChatMessage("Takaro", message)`, invokes the existing routed RPC, and advances the sequence only after successful invocation. Inject a late-bound adapter delegate from `ValheimTakaroPlugin` to avoid the existing adapter/runner/bridge construction cycle.

Direct delivery returns `companion_server_chat_unavailable` if the peer cannot receive chat. Global delivery returns `{ sent, recipients, skipped }` and an error when zero compatible peers receive it. Remove the overlay path only from `SendMessageAsync`; preserve `SendHudMessage` for item confirmation.

**Step 4: Build the real server plugin and run focused tests**

Run:

```bash
BEPINEX_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/BepInEx/core \
VALHEIM_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed \
dotnet build valheim/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj -f net472 -p:EnableValheimPluginBuild=true -v minimal
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj --filter "CompanionServerBridgeContractTests|PluginScaffoldContractTests" -v minimal
```

Expected: real plugin build and focused tests pass.

**Step 5: Commit**

```bash
git add valheim/src/Takaro.Valheim.Plugin valheim/tests/Takaro.Valheim.Core.Tests
git commit -m "fix(valheim): route server messages to chat"
```

### Task 5: Correct documentation, capability claims, and packaging tests

**Files:**
- Modify: `valheim/README.md`
- Modify: `valheim/capabilities.json`
- Modify: `valheim/qa/2026-07-12-owned-companion-validation.md`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionDocumentationContractTests.cs`
- Modify: `valheim/tests/release-package-behavior.sh`

**Step 1: Write failing documentation/package assertions**

Require documentation to state normal chat through the required companion and reject claims that `sendMessage` uses HUD calls. Require both packaged roles to carry the same protocol assembly containing `server-chat` and the companion package to contain the renderer.

**Step 2: Run focused checks and verify RED**

Run:

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj --filter CompanionDocumentationContractTests -v minimal
```

Expected: FAIL on the old HUD documentation.

**Step 3: Update documentation honestly**

Describe the client-owned chat renderer, `ServerChat` capability, no-overlay behavior, compatibility failure, and exact live-proof gate. Keep `sendMessage` `live-supported` only after the final live run succeeds.

**Step 4: Run complete verification and package checks**

Run:

```bash
dotnet test valheim/Takaro.Valheim.sln -v minimal
VALHEIM_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed \
BEPINEX_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/BepInEx/core \
bash valheim/scripts/build-release.sh 2.0.1 /tmp/valheim-chat-release
bash valheim/tests/release-package-behavior.sh 2.0.1 /tmp/valheim-chat-release
```

Expected: full tests and separate server/client archive validation pass.

**Step 5: Commit**

```bash
git add valheim/README.md valheim/capabilities.json valheim/qa valheim/tests
git commit -m "docs(valheim): require chat-visible server messages"
```

### Task 6: Install and live-prove chat-only delivery

**Files:**
- Runtime only: `/home/hendrik/valheim-dedicated-server/BepInEx/plugins/TakaroValheim`
- Runtime only: `/home/hendrik/.local/share/Steam/steamapps/common/Valheim/BepInEx/plugins/TakaroValheimCompanion`
- Evidence: `/tmp/valheim-chat-live-proof/`

**Step 1: Stop and replace both exact runtime roles**

Gracefully stop the current Valheim client/server, back up the installed v2.0.0 folders, install the exact packaged 2.0.1 artifacts, and record SHA-256 hashes.

**Step 2: Restart server and client cleanly**

Verify BepInEx logs load the new product version on both sides and negotiate `ServerChat`. Do not reuse stale log lines.

**Step 3: Prove direct and global actions**

Use Takaro MCP `gameserverSendMessage` for one direct and one global unique marker. Confirm server logs show companion chat delivery and the player confirms both markers appear as `Takaro` lines in the chat window with no center/top-left overlay.

**Step 4: Prove installed module automation**

Trigger or wait for an installed `serverMessages` module cronjob. Confirm the module run/event in Takaro, connector delivery in the server log, and the exact marker in normal chat.

**Step 5: Record the live evidence and commit**

Update `valheim/qa/2026-07-12-owned-companion-validation.md` with timestamps, artifact hashes, Takaro IDs, log markers, visual evidence path, and the chat-only verdict. Commit the ledger.

### Task 7: Review, merge, release, and re-smoke published assets

**Files:**
- No new source files expected.

**Step 1: Run branch verification**

Run `git diff --check`, full Valheim tests, real plugin builds, release packaging, secret scan, and an independent code review. Fix findings test-first and rerun affected/full gates.

**Step 2: Push and open the PR**

Use the repository PR workflow with a conventional title. Include the observed overlay defect, owned companion route, test evidence, and live player-visible chat proof.

**Step 3: Monitor CI and merge**

Require `shellcheck`, `cleanup`, `conventional-title`, `test`, and `package` to pass before squash merge.

**Step 4: Merge the release PR and validate published assets**

Require the Valheim release workflow to publish both role-separated archives. Download the official assets, verify manifests/hashes/contents, and run `release-package-behavior.sh` against them.

**Step 5: Final released-asset smoke**

Install the official published server and companion archives, restart cleanly, reconnect, and send a unique Takaro message. Completion requires a normal `Takaro` chat line and no HUD overlay.
