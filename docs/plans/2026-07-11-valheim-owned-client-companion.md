# Takaro-Owned Valheim Client Companion Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build, package, and live-prove a Takaro-owned Valheim companion required on every player client, adding client-backed chat, commands, inventory, player-death, and attributed entity-kill coverage without exposing Takaro credentials.

**Architecture:** Add a neutral versioned protocol library, a graphical-client-only BepInEx companion, and an optional/required companion bridge inside the existing dedicated-server connector. The client sends bounded reports over Valheim `ZRoutedRpc`; the server binds them to the real RPC sender, validates/rate-limits/deduplicates them, and alone communicates with Takaro.

**Tech Stack:** C#/.NET 8 + .NET Standard 2.0 + .NET Framework 4.7.2, BepInEx 5, Harmony, Valheim `ZRoutedRpc`, System.Text.Json, MSTest, Bash, GitHub Actions, Takaro MCP, Steam/Valheim graphical client automation.

---

## Execution Rules

- Work only on branch `feat/valheim-owned-companion` in `/home/hendrik/.config/superpowers/worktrees/connectors/valheim-server-only-consolidated`.
- Treat `docs/plans/2026-07-11-valheim-owned-client-companion-design.md` as the approved source of truth.
- Follow red-green-refactor for every behavior. Never add production behavior before observing its targeted test fail for the intended reason.
- Use `apply_patch` for repository edits. Stage only files named by the current task.
- The companion project may reference only the neutral protocol project and game/BepInEx assemblies. It must never reference `Takaro.Valheim.Core` or contain server credentials/cloud transport.
- Do not cherry-pick PR #79. Port only reviewed ideas into the hardened current main.
- Do not promote a capability to `live-supported` until the exact packaged server and client artifacts pass the corresponding live gate.
- Preserve all unrelated processes and user state. In particular, never stop or alter active Conan or Palworld sessions.
- Run kick, ban, unban, and shutdown only on the local disposable Valheim profile, restore ban/config/plugin/world state afterward, and run shutdown last.
- Never print registration tokens, identity tokens, raw MCP traffic, or secret-bearing config.

### Task 1: Add the Neutral Companion Protocol Project

**Files:**
- Create: `valheim/src/Takaro.Valheim.Companion.Protocol/Takaro.Valheim.Companion.Protocol.csproj`
- Create: `valheim/src/Takaro.Valheim.Companion.Protocol/Compatibility/IsExternalInit.cs`
- Create: `valheim/src/Takaro.Valheim.Companion.Protocol/CompanionProtocol.cs`
- Create: `valheim/src/Takaro.Valheim.Companion.Protocol/CompanionMessages.cs`
- Modify: `valheim/Takaro.Valheim.sln`
- Modify: `valheim/src/Takaro.Valheim.Core/Takaro.Valheim.Core.csproj`
- Modify: `valheim/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionProtocolTests.cs`

**Step 1: Scaffold the empty project and solution references**

Create a multi-target protocol project with nullable/latest language settings and `System.Text.Json 8.0.5`. Add it to the solution and reference it from Core, server plugin, and tests. Do not add behavior yet.

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFrameworks>net8.0;netstandard2.0</TargetFrameworks>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
    <LangVersion>latest</LangVersion>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="System.Text.Json" Version="8.0.5" />
  </ItemGroup>
</Project>
```

**Step 2: Write failing protocol-shape tests**

Cover stable RPC name/version, capabilities, identity-free report payloads, and bounds.

```csharp
[TestMethod]
public void VersionOneReportsContainNoClaimedPlayerIdentity()
{
    var reportTypes = new[]
    {
        typeof(CompanionChatReport),
        typeof(CompanionInventoryReport),
        typeof(CompanionPlayerDeathReport),
        typeof(CompanionEntityKilledReport)
    };

    foreach (var type in reportTypes)
    {
        var names = type.GetProperties().Select(property => property.Name).ToArray();
        CollectionAssert.DoesNotContain(names, "PlayerId", type.Name);
        CollectionAssert.DoesNotContain(names, "GameId", type.Name);
        CollectionAssert.DoesNotContain(names, "SteamId", type.Name);
    }
}
```

**Step 3: Run the focused test and observe RED**

Run:

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter FullyQualifiedName~CompanionProtocolTests -v minimal
```

Expected: compilation fails because `CompanionProtocol` and message types do not exist.

**Step 4: Implement the minimal protocol constants and DTOs**

Use one RPC and a single envelope. Reports must not carry identity.

```csharp
public static class CompanionProtocol
{
    public const string RpcName = "TakaroCompanionV1";
    public const int CurrentVersion = 1;
    public const int MinimumVersion = 1;
    public const int MaximumEnvelopeUtf8Bytes = 64 * 1024;
    public const int MaximumChatCharacters = 512;
    public const int MaximumInventoryStacks = 256;
    public const int MaximumCodeCharacters = 128;
    public const int MaximumEventIdCharacters = 64;
}

[Flags]
public enum CompanionCapability
{
    None = 0,
    Chat = 1,
    Inventory = 2,
    PlayerDeath = 4,
    EntityKilled = 8
}

public sealed record CompanionEnvelope(
    int ProtocolVersion,
    string SessionNonce,
    long Sequence,
    string MessageId,
    string Type,
    JsonElement Payload);
```

Add `hello`, `hello-ack`, `heartbeat`, `chat`, `inventory-snapshot`, `player-death`, and `entity-killed` payload records. Inventory entries carry code/name/amount/quality/durability/equipped/slot only. Death/kill reports carry event ID, timestamp, position, and bounded attacker/entity/weapon hints, never player identity.

**Step 5: Run focused and full protocol-target tests**

Run the focused command again, then:

```bash
dotnet test valheim/Takaro.Valheim.sln --no-restore -v minimal
```

Expected: all tests pass.

**Step 6: Commit**

```bash
git add valheim/Takaro.Valheim.sln \
  valheim/src/Takaro.Valheim.Companion.Protocol \
  valheim/src/Takaro.Valheim.Core/Takaro.Valheim.Core.csproj \
  valheim/src/Takaro.Valheim.Plugin/Takaro.Valheim.Plugin.csproj \
  valheim/tests/Takaro.Valheim.Core.Tests
git commit -m "feat(valheim): add owned companion protocol"
```

### Task 2: Add Strict Envelope Encoding and Version Negotiation

**Files:**
- Create: `valheim/src/Takaro.Valheim.Companion.Protocol/CompanionEnvelopeCodec.cs`
- Create: `valheim/src/Takaro.Valheim.Companion.Protocol/CompanionVersionPolicy.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionProtocolTests.cs`

**Step 1: Add failing codec and negotiation tests**

Add tests named:

- `RoundTripsEveryVersionOneMessage`
- `RejectsEnvelopeOverMaximumUtf8BytesBeforeJsonParsing`
- `RejectsUnknownMessageTypeAndProtocolVersion`
- `NegotiatesHighestOverlappingProtocolVersion`
- `RejectsNonOverlappingProtocolVersionRange`
- `RejectsMissingNonceMessageIdAndNonPositiveSequence`
- `RejectsUnknownPayloadFieldsThatCouldClaimIdentity`

```csharp
[TestMethod]
public void NegotiatesHighestOverlappingProtocolVersion()
{
    Assert.IsTrue(CompanionVersionPolicy.TryNegotiate(1, 3, 2, 4, out var selected));
    Assert.AreEqual(3, selected);
}
```

**Step 2: Verify RED**

Run the Task 1 focused test command. Expected: missing codec/policy failures.

**Step 3: Implement bounded codec and version policy**

`TryDecodeEnvelope` must count UTF-8 bytes before deserializing, reject unknown top-level fields, validate known message types, and return a stable short error code without echoing payload contents. `TryDecodePayload<T>` must use strict per-message validation.

```csharp
public static bool TryNegotiate(
    int localMinimum,
    int localMaximum,
    int remoteMinimum,
    int remoteMaximum,
    out int selected)
{
    selected = Math.Min(localMaximum, remoteMaximum);
    return selected >= Math.Max(localMinimum, remoteMinimum);
}
```

**Step 4: Verify GREEN and commit**

Run focused then full tests. Commit:

```bash
git add valheim/src/Takaro.Valheim.Companion.Protocol \
  valheim/tests/Takaro.Valheim.Core.Tests/CompanionProtocolTests.cs
git commit -m "feat(valheim): validate companion wire protocol"
```

### Task 3: Implement Deterministic Companion Sessions

**Files:**
- Create: `valheim/src/Takaro.Valheim.Core/CompanionSessionRegistry.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionSessionRegistryTests.cs`

**Step 1: Write failing session tests**

Cover:

- `ReportBeforeHelloAckIsRejected`
- `HelloAckMustEchoCurrentNonce`
- `ReconnectReplacesNonceAndRejectsOldSession`
- `SequenceMustIncreaseWithinSession`
- `HeartbeatRefreshesOnlyNegotiatedSession`
- `RequiredSessionExpiresAfterHandshakeGrace`
- `HeartbeatExpiresAfterGrace`
- `RemovePeerAndSwitchWorldClearSessionState`

Inject timestamps and nonces; tests must not sleep or use random global state.

```csharp
var hello = sessions.Begin(peerId: 42, now, nonce: "nonce-a");
Assert.AreEqual("nonce-a", hello.Nonce);
Assert.AreEqual(CompanionSessionDecision.RejectNotNegotiated,
    sessions.ValidateReport(42, "nonce-a", sequence: 1, now));
```

**Step 2: Verify RED**

Run:

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter FullyQualifiedName~CompanionSessionRegistryTests -v minimal
```

**Step 3: Implement the thread-safe registry**

Key the dictionary by peer UID. Store nonce, selected protocol, product version, capabilities, last sequence, handshake deadline, and last heartbeat. Expose only explicit decisions; no game API calls belong here.

```csharp
public enum CompanionSessionDecision
{
    Accept,
    RejectUnknownPeer,
    RejectNotNegotiated,
    RejectNonce,
    RejectSequence,
    RejectVersion,
    Expired
}
```

Lock all compound session operations. `SwitchWorld(object identity)` clears sessions atomically when the identity changes.

**Step 4: Verify GREEN and commit**

Run focused/full tests and commit `feat(valheim): track companion sessions`.

### Task 4: Add Rate Limits, Deduplication, and Fresh Inventory Cache

**Files:**
- Create: `valheim/src/Takaro.Valheim.Core/CompanionRateLimiter.cs`
- Create: `valheim/src/Takaro.Valheim.Core/BoundedEventDeduplicator.cs`
- Create: `valheim/src/Takaro.Valheim.Core/CompanionInventoryCache.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionValidationTests.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionInventoryCacheTests.cs`

**Step 1: Write failing guard/cache tests**

Required tests:

- `RateLimitsArePerPeerAndMessageType`
- `RateLimitRefillsFromInjectedTime`
- `DuplicateEventIdIsAcceptedExactlyOnce`
- `DeduplicatorEvictsOldestEntryAtBound`
- `InventoryAcceptsConfirmedEmptySnapshot`
- `InventoryRejectsOversizedMalformedOrNegativeStacks`
- `OlderSnapshotCannotOverwriteNewerSnapshot`
- `ExpiredInventoryReturnsUnavailableInsteadOfFabricatedEmpty`
- `RemovePeerAndWorldResetClearInventoryAliases`

**Step 2: Verify RED**

Run filters for `CompanionValidationTests|CompanionInventoryCacheTests`.

**Step 3: Implement minimal thread-safe primitives**

The cache stores canonical immutable arrays keyed by server-derived player aliases plus peer/session identity, with a default 30-second TTL. It distinguishes a confirmed fresh empty inventory from unavailable/expired data.

```csharp
public enum CompanionInventoryState { Fresh, Missing, Expired }

public CompanionInventoryState TryGet(
    string identifier,
    DateTimeOffset now,
    out IReadOnlyList<TakaroInventoryItem> items);
```

Use a bounded token bucket per `(peerId, messageType)` and a bounded `(peerId, eventId)` cache.

**Step 4: Verify GREEN and commit**

Run focused/full tests and commit `feat(valheim): guard companion reports`.

### Task 5: Process Client Reports into Exact Takaro Shapes

**Files:**
- Create: `valheim/src/Takaro.Valheim.Core/CompanionReportProcessor.cs`
- Modify: `valheim/src/Takaro.Valheim.Core/Events.cs`
- Modify: `valheim/src/Takaro.Valheim.Core/ServerOnlyPolicies.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionReportProcessorTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/EventFactoryTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/TakaroConsumerContractTests.cs`

**Step 1: Write failing processor/schema tests**

Cover authoritative sender binding and exact Takaro schemas:

- `ChatUsesAuthoritativeRpcSenderPlayer`
- `InventoryUpdatesCacheWithoutEmittingAnEvent`
- `PlayerDeathEmitsExactlyOnceWithTakaroSchema`
- `EntityKilledUsesPlayerEntityWeaponAndTimestampShape`
- `InvalidStaleDuplicateAndRateLimitedReportsEmitNothing`

```csharp
Assert.AreEqual("Steam_real", data.GetProperty("player").GetProperty("gameId").GetString());
Assert.AreEqual("Greydwarf", data.GetProperty("entity").GetString());
Assert.AreEqual("SwordIron", data.GetProperty("weapon").GetString());
```

Do not accept a payload player ID. A Valheim skill (`hit.m_skill`) is not a weapon; the client must send the current weapon prefab/display name or omit it.

**Step 2: Verify RED**

Run filters for `CompanionReportProcessorTests|EventFactoryTests|TakaroConsumerContractTests`.

**Step 3: Implement the report processor and event policy**

Add `ClientCompanion` to `ValheimEventObservationSource`. Permit only chat, death, and entity-killed from that source. The processor receives the server-resolved `TakaroPlayer`, validated envelope, session/guard/cache dependencies, and current time; return either no output, an inventory update, or a typed accepted event.

**Step 4: Verify GREEN and commit**

Run focused/full tests and commit `feat(valheim): process companion reports`.

### Task 6: Integrate Fresh Companion Inventory into the Server Adapter

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Core/ConnectorConfig.cs`
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/ConfigTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionAdapterContractTests.cs`

**Step 1: Write failing config and inventory tests**

Add `disabled|optional|required` parsing, default `required`, default `$` command prefix, and invalid-value rejection. Add adapter contracts for fresh, confirmed-empty, missing, and expired inventory.

```csharp
Assert.AreEqual(CompanionMode.Required, config.CompanionMode);
CollectionAssert.AreEqual(new[] { "$" }, config.CompanionCommandPrefixes.ToArray());
```

**Step 2: Verify RED**

Run `ConfigTests|CompanionAdapterContractTests|PluginScaffoldContractTests`.

**Step 3: Implement minimal config/cache injection**

Inject `CompanionInventoryCache` into the real adapter constructor. `GetPlayerInventoryAsync` must first resolve the player through server-owned identity, then return only a fresh cache value. Keep `player_component_unavailable` for disabled, missing, or stale companion state; Takaro's existing array-failure suppression remains intact.

**Step 4: Verify GREEN and commit**

Run focused/full tests and commit `feat(valheim): serve fresh companion inventory`.

### Task 7: Extract Authoritative Valheim Peer Resolution

**Files:**
- Create: `valheim/src/Takaro.Valheim.Plugin/ValheimPlayerResolver.cs`
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimServerAdapter.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`

**Step 1: Add failing structural contracts**

Assert that one resolver owns `ZNet.PlayerInfo`/`ZNetPeer` conversion and that neither server adapter nor later companion bridge trusts payload identity.

**Step 2: Verify RED**

Run `PluginScaffoldContractTests` and require a missing `ValheimPlayerResolver` failure.

**Step 3: Extract without changing behavior**

Move the current `ToTakaroPlayer`, `TryResolvePlayer`, `TryFindPlayerInfo`, and `TryFindPeer` behavior into the resolver. Keep all existing aliases and platform ID normalization. Inject it into the adapter.

**Step 4: Verify GREEN, full regression, and commit**

Run all tests. Commit `refactor(valheim): centralize peer identity resolution`.

### Task 8: Add the Dedicated-Server Companion Bridge

**Files:**
- Create: `valheim/src/Takaro.Valheim.Plugin/CompanionServerBridge.cs`
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimChatEventBridge.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionServerBridgeContractTests.cs`

**Step 1: Write failing bridge contracts**

Required contracts:

- `ServerRegistersOneBoundedEnvelopeRpc`
- `ServerTargetsHelloToTheExactPeer`
- `ServerBindsReportsToTheRpcSender`
- `ServerRejectsUnknownOrNotReadyPeer`
- `ServerForwardsOnlyAcceptedEvents`
- `WorldChangeAndDisconnectClearCompanionState`
- `OldRoutedChatDiagnosticsRemainNonEmitting`

**Step 2: Verify RED**

Run bridge/scaffold filters.

**Step 3: Implement the real bridge**

On `Update()`, register `ZRoutedRpc.Register<string>(CompanionProtocol.RpcName, handler)` once per routed-RPC instance, discover ready peers, begin sessions, and send hello to `peer.m_uid`. The handler maps `sender` to an actual connected peer, resolves the authoritative player, decodes/validates, processes, and forwards accepted events through `TakaroWebSocketRunner.SendGameEventAsync`.

The client-to-server target must be `ZRoutedRpc.instance.GetServerPeerID()`. Never broadcast. Keep every Unity/Valheim API access on the plugin `Update()`/RPC main-thread boundary.

**Step 4: Verify GREEN and commit**

Run focused/full tests and commit `feat(valheim): receive owned companion reports`.

### Task 9: Enforce Missing, Mismatched, and Silent Companions

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Plugin/CompanionServerBridge.cs`
- Modify: `valheim/src/Takaro.Valheim.Plugin/ValheimTakaroPlugin.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionServerBridgeContractTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`

**Step 1: Add failing enforcement tests**

Cover:

- `DisabledModeRegistersNothing`
- `OptionalModeExpiresCapabilitiesButNeverDisconnects`
- `RequiredModeExplainsThenDisconnectsMissingCompanion`
- `RequiredModeExplainsExpectedAndActualVersion`
- `RequiredModeDisconnectsExpiredHeartbeatAfterGrace`
- `CompatibleProductPatchDoesNotFailWireCompatibility`

**Step 2: Verify RED**

Run focused filters.

**Step 3: Integrate bridge lifecycle and enforcement**

Bind companion settings in `Awake`, construct cache/resolver/adapter/runner/bridge in that order, call bridge `Update()` after `mainThreadActions.Drain()`, and dispose it before runner/cache teardown. In required mode, use built-in Valheim player-visible messaging before a delayed main-thread disconnect. Confirm the exact disconnect API against current assemblies; do not assume a `Kicked` transport acknowledgement proves visible rejection.

**Step 4: Verify GREEN and commit**

Run focused/full tests and commit `feat(valheim): enforce companion compatibility`.

### Task 10: Add the Graphical-Client Companion Plugin

**Files:**
- Create: `valheim/src/Takaro.Valheim.Companion/Takaro.Valheim.Companion.csproj`
- Create: `valheim/src/Takaro.Valheim.Companion/ValheimCompanionPlugin.cs`
- Create: `valheim/src/Takaro.Valheim.Companion/CompanionClientBridge.cs`
- Modify: `valheim/Takaro.Valheim.sln`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionPluginContractTests.cs`

**Step 1: Scaffold project/solution wiring only**

Target `net8.0;net472`, assembly name `Takaro.Valheim.Companion`, conditional `EnableValheimCompanionBuild`, and the same real BepInEx/Harmony/Valheim references as the server plugin. Reference only `Takaro.Valheim.Companion.Protocol`.

**Step 2: Write failing ownership/process tests**

Cover:

- `CompanionLoadsOnlyInGraphicalValheimClient`
- `CompanionRefusesDedicatedServerProcess`
- `CompanionReferencesProtocolButNeverCore`
- `CompanionContainsNoTakaroCredentialsOrCloudTransport`
- `CompanionUsesNoJotunnOrServerSync`
- `ServerPluginStillRefusesGraphicalClient`

**Step 3: Verify RED**

Run `CompanionPluginContractTests`.

**Step 4: Implement the minimal BepInEx entrypoint**

Use GUID `com.takaro.valheim.companion`. Add generated product/protocol version constants. In `Awake`, disable on batch/dedicated process, patch only the companion assembly, initialize the client bridge, and log no secrets. In `Update`, call the bridge; in `OnDestroy`, unpatch/reset.

**Step 5: Verify GREEN and commit**

Run focused/full tests and commit `feat(valheim): add owned client companion`.

### Task 11: Implement Client Negotiation and Heartbeats

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Companion/CompanionClientBridge.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionPluginContractTests.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionClientStateTests.cs`

**Step 1: Add failing client-state tests/contracts**

Cover `ClientCannotReportBeforeHello`, nonce echo, highest compatible version, exact server target, monotonic sequence, heartbeat interval, routed-RPC/world reset, and stale session rejection.

**Step 2: Verify RED**

Run client state/contract filters.

**Step 3: Implement negotiation**

Register the same envelope RPC when `ZRoutedRpc.instance` changes. Accept only server hello messages, echo nonce/version/product/capabilities, target only `GetServerPeerID()`, and enable reports only after a valid ack. Send heartbeat every five seconds while connected and reset all state on disconnect/world/RPC replacement.

**Step 4: Verify GREEN and commit**

Commit `feat(valheim): negotiate companion sessions` after focused/full tests.

### Task 12: Forward Ordinary Chat and Commands Exactly Once

**Files:**
- Create: `valheim/src/Takaro.Valheim.Companion/CompanionChatPolicy.cs`
- Create: `valheim/src/Takaro.Valheim.Companion/CompanionClientHooks.cs`
- Modify: `valheim/src/Takaro.Valheim.Companion/CompanionClientBridge.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionChatPolicyTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionPluginContractTests.cs`

**Step 1: Write failing chat tests**

Cover ordinary chat continues through Valheim and reports once; `$` command reports once and suppresses ordinary game chat; remote talkers/blank/oversized text are ignored; configured prefix boundaries are exact.

**Step 2: Verify RED**

Run chat/contract filters.

**Step 3: Implement the `Talker.Say` patch**

Port the proven PR #79 integration point, not its whole bridge. Use a Prefix to detect local-player commands and suppress the original only for accepted commands. Use a Postfix/state for ordinary local chat so it is forwarded once after normal Valheim behavior. Emit one bounded `CompanionChatReport`.

**Step 4: Verify GREEN and commit**

Commit `feat(valheim): forward client chat and commands` after full tests.

### Task 13: Send Changed Inventory Snapshots Only

**Files:**
- Create: `valheim/src/Takaro.Valheim.Companion/CompanionInventoryReader.cs`
- Modify: `valheim/src/Takaro.Valheim.Companion/CompanionClientBridge.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionInventoryReaderContractTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionPluginContractTests.cs`

**Step 1: Add failing inventory contracts**

Cover initial ready snapshot, canonical ordering/hash, no resend when unchanged, resend after mutation, confirmed empty snapshot, bounds, and reset/reconnect behavior.

**Step 2: Verify RED**

Run inventory filters.

**Step 3: Implement bounded polling**

From the companion `Update`, poll `Player.m_localPlayer` readiness at a modest interval. Map `GetInventory().GetAllItems()` to neutral protocol items, canonicalize/sort, hash, send immediately after negotiation, then only when the hash changes. Do not serialize inventory every frame and do not log contents.

**Step 4: Verify GREEN and commit**

Commit `feat(valheim): report client inventory changes` after full tests.

### Task 14: Emit Local Player Death and Attributed Entity Kills

**Files:**
- Modify: `valheim/src/Takaro.Valheim.Companion/CompanionClientHooks.cs`
- Create: `valheim/src/Takaro.Valheim.Companion/CompanionCombatReader.cs`
- Modify: `valheim/src/Takaro.Valheim.Companion/CompanionClientBridge.cs`
- Test: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionCombatContractTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CompanionPluginContractTests.cs`

**Step 1: Add failing combat contracts**

Cover one event ID per local death, remote player death ignored, non-player entity only, `m_lastHit.GetAttacker() == Player.m_localPlayer`, real weapon prefab/display extraction, missing weapon omission, and duplicate Harmony callbacks suppressed.

**Step 2: Verify RED**

Run combat filters.

**Step 3: Implement minimal hooks**

Use Harmony postfixes on `Player.OnDeath` and `Character.OnDeath`. Guard local-player ownership first. Read `m_lastHit` through a cached Harmony `AccessTools` field accessor if not public. For entity kills, require the local player as attacker and exclude players. Generate bounded unique event IDs and send only after a negotiated session.

**Step 4: Verify GREEN and commit**

Commit `feat(valheim): report client death and kills` after full tests.

### Task 15: Build and Verify Two Reproducible Release Archives

**Files:**
- Create: `valheim/tests/release-package-behavior.sh`
- Create: `valheim/tests/Takaro.Valheim.Core.Tests/ReleasePackageContractTests.cs`
- Modify: `valheim/scripts/build-release.sh`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/ReleaseVersionContractTests.cs`
- Modify: `.github/workflows/valheim.yml`

**Step 1: Write the failing package harness**

The fixture harness must reject missing/wrong-role DLLs, server/Core/config files in the client ZIP, PDB/deps/host/Jotunn files, secret/cloud markers, and mismatched product/protocol manifests. Scan extracted bytes with `rg -a`, never the compressed ZIP.

```bash
for marker in registrationToken identityToken takaroWsUrl connect.takaro.io \
  ClientWebSocket TakaroWebSocketRunner ValheimServerAdapter; do
  if rg -a -q "$marker" "$client_extract/TakaroValheimCompanion"; then
    printf 'client artifact contains banned marker: %s\n' "$marker" >&2
    exit 1
  fi
done
```

**Step 2: Verify RED**

Run:

```bash
dotnet test valheim/tests/Takaro.Valheim.Core.Tests/Takaro.Valheim.Core.Tests.csproj \
  --filter FullyQualifiedName~ReleasePackageContractTests -v minimal
```

Expected: current one-archive build violates the new contract.

**Step 3: Publish two separate projects**

Produce:

- `takaro-valheim-plugin.zip`: server DLL, Core, Protocol, required runtime DLLs, server README/manifest.
- `takaro-valheim-companion.zip`: companion DLL, Protocol, required runtime DLLs, client README/manifest.

Strip host/reference/Jotunn/test/source/debug files from both. Include product version, numeric BepInEx version, process role, and protocol min/current/max. Use `SOURCE_DATE_EPOCH`, normalized stage mtimes, sorted input, and `zip -X` so two clean builds hash identically.

**Step 4: Publish both assets in CI**

Pass both files to existing multi-file `publish-release.sh` and `comment-pr-build.sh` calls for stable, rolling, and PR builds. No generic script change is required.

**Step 5: Verify GREEN locally**

```bash
dotnet restore valheim/Takaro.Valheim.sln
dotnet test valheim/Takaro.Valheim.sln --no-restore -v minimal
bash valheim/tests/setup-environment-behavior.sh

VERSION="2.0.0-rc.1+package"
VALHEIM_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/valheim_server_Data/Managed \
BEPINEX_REFERENCE_PATH=/home/hendrik/valheim-dedicated-server/BepInEx/core \
  valheim/scripts/build-release.sh "$VERSION" /tmp/valheim-companion-dist-a

bash valheim/tests/release-package-behavior.sh \
  "$VERSION" /tmp/valheim-companion-dist-a
```

Also build the companion against graphical-client references:

```bash
dotnet build valheim/src/Takaro.Valheim.Companion/Takaro.Valheim.Companion.csproj \
  -c Release -f net472 -p:EnableValheimCompanionBuild=true \
  -p:BepInExReferencePath=/home/hendrik/.local/share/Steam/steamapps/common/Valheim/BepInEx/core \
  -p:ValheimReferencePath=/home/hendrik/.local/share/Steam/steamapps/common/Valheim/valheim_Data/Managed
```

**Step 6: Commit**

Commit `build(valheim): package owned client companion`.

### Task 16: Document Installation Without Overclaiming

**Files:**
- Create: `valheim/COMPANION.md`
- Modify: `valheim/README.md`
- Modify: `valheim/capabilities.json`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CapabilityRegistryTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/PluginScaffoldContractTests.cs`

**Step 1: Add failing documentation/registry contracts**

Require two-package install/upgrade/removal instructions, required mode behavior, no client tokens, trust warning, protocol compatibility, and retained unsupported statuses before live proof. Replace conflicting server-only assertions narrowly; keep the server connector graphical-client prohibition.

**Step 2: Verify RED**

Run capability/scaffold tests.

**Step 3: Write truthful pre-live docs**

Document the companion as implemented but not yet live-supported. Add ownership/source metadata (`server-owned`, `client-reported`, `upstream-blocked`, `unsupported`) without replacing the allowed support statuses. Keep map unsupported and `listLocations` schema-fallback/upstream-blocked.

**Step 4: Verify GREEN and commit**

Commit `docs(valheim): document owned companion rollout` after full tests.

### Task 17: Run Full Automated and Independent Review Gates

**Files:**
- Modify only files required by verified findings.

**Step 1: Run exact automated gates**

```bash
dotnet restore valheim/Takaro.Valheim.sln
dotnet test valheim/Takaro.Valheim.sln --no-restore -v minimal
bash valheim/tests/setup-environment-behavior.sh
bash -n valheim/scripts/*.sh valheim/tests/*.sh
docker run --rm -v "$PWD:/mnt" -w /mnt koalaman/shellcheck:stable \
  -e SC1091 $(git ls-files '*.sh')
git diff --check origin/main...HEAD
jq empty valheim/capabilities.json
```

Build server and client `net472` against their real reference boundaries and build/package twice with identical `SOURCE_DATE_EPOCH`; compare both ZIP hashes.

**Step 2: Run `/verify --mode=report-only --scope=branch`**

Require static analysis, tests, QA, connector coverage review, package inspection, independent code review, and no severity-5-or-higher finding. Feed every qualifying finding into a new red/green fix turn, then rerun the full branch scope.

**Step 3: Commit only verified fixes**

Use focused conventional commits. Do not change capability statuses yet.

### Task 18: Prove Required Compatibility with Exact Packages

**Files:**
- Create/update after proof: `valheim/qa/2026-07-11-owned-companion-validation.md`
- Evidence directory (untracked): `/tmp/valheim-companion-v2-evidence`

**Step 1: Prepare disposable state and exact artifacts**

- Confirm Valheim server/client are stopped and Takaro MCP is healthy at `127.0.0.1:3000`.
- Record active Conan/Palworld sessions and leave them untouched.
- Back up server config/plugins/world and client plugins without printing config.
- Temporarily isolate Jotunn from both Valheim processes so BepInEx-only operation is real.
- Build `2.0.0-rc.1+verify`, record both ZIP/DLL hashes, and install the exact server ZIP.

**Step 2: Live-prove missing companion rejection**

Set required mode, leave the client companion absent, start the dedicated server with the known test command, and join using `scripts/valheim-ui-connect-client.sh --kill-client`. Require a player-visible explanation, server rejection log, no admitted companion session, and no fabricated Takaro events/state.

**Step 3: Live-prove incompatible companion rejection**

Build/install a test-only incompatible protocol fixture while keeping the exact server artifact. Require expected/actual versions in logs/UI and rejection before capability-backed state is accepted.

**Step 4: Live-prove a compatible session**

Install the exact companion ZIP, reconnect, and prove matching installed hashes, graphical-client/server process roles, nonce exchange, negotiated protocol/capabilities, heartbeat, identity from real peer, reconnect, and server restart with no stale session replay.

**Step 5: Record evidence and clean interim state**

Sanitize logs/screenshots into the evidence directory. Do not promote capabilities yet; feature behavior remains to be exercised.

### Task 19: Prove Client-Backed Features and Takaro Modules Live

**Files:**
- Update: `valheim/qa/2026-07-11-owned-companion-validation.md`

**Step 1: Prove ordinary chat and commands**

Use `xdotool` to type one ordinary message and one `$tplist`/safe teleport command. Require exactly one Takaro `chat-message` for each accepted input, exactly one installed-module execution, server bridge logs bound to the real peer, and real player location change for teleport. Prove commands are not duplicated into normal chat.

**Step 2: Prove inventory mutation**

Resolve player `Hehe`/Takaro UUID through MCP. Confirm the initial real inventory, create one Wood world drop through `gameserverGiveItem`, collect it in the client, and require a real inventory delta in Takaro. Prove a confirmed empty inventory separately only if using a disposable empty character; never clear or fabricate the normal character inventory.

**Step 3: Prove death and entity kill**

Trigger one controlled real player death and require one schema-valid persisted `player-death`. Spawn/engage one disposable Greyling and kill it with a real local-player weapon; require exactly one `entity-killed` with player, entity, weapon, and timestamp. Record game/client/server logs plus Takaro event IDs.

**Step 4: Prove abuse rejection**

Use a test harness against the server bridge for wrong nonce, claimed identity field, duplicate event ID, stale sequence, oversized chat/inventory, and rate-limit burst. Require bounded rejection logs and zero Takaro persistence.

**Step 5: Re-run server-owned and automation regressions**

Through MCP and installed modules, re-prove reachability, players, `getPlayer`, location, teleport, world-drop `giveItem`, messages, item/entity catalogs, raw `listLocations`, lifecycle, server-message cron, and command hook delivery.

### Task 20: Prove Moderation/Shutdown, Finalize Truth, and Ship

**Files:**
- Modify: `valheim/capabilities.json`
- Modify: `valheim/README.md`
- Modify: `valheim/COMPANION.md`
- Modify: `valheim/qa/2026-07-10-server-only-validation.md`
- Modify: `valheim/qa/2026-07-11-owned-companion-validation.md`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/CapabilityRegistryTests.cs`
- Modify: `valheim/tests/Takaro.Valheim.Core.Tests/QaLedgerContractTests.cs`

**Step 1: Exercise approval-gated server actions on disposable state**

Run kick and prove disconnect; reconnect. Run ban, verify Takaro ban state and Valheim's official `bannedlist.txt`, prove rejected reconnect, then unban and prove both stores clear plus reconnect. Run shutdown last and require process exit plus Takaro unreachable state. Restore all ban/config/plugin/world/client state and confirm unrelated sessions remained untouched.

**Step 2: Promote only live-proven capabilities**

Update chat, inventory, player-death, entity-killed, `getPlayer`, and destructive actions only where exact artifact evidence passed. Keep any failed/unexercised item unsupported. Keep map unsupported and `listLocations` schema-fallback/upstream-blocked. Replace the stale turn-9/turn-10-pending ledger headline with final historical truth and link the new companion ledger.

**Step 3: Run the entire verification suite again**

Repeat Task 17, rebuild both artifacts, and ensure docs/tests contain exact final hashes/event IDs without secrets. Run independent review and exact package inspection one last time.

**Step 4: Commit final proof**

```bash
git add valheim/capabilities.json valheim/README.md valheim/COMPANION.md \
  valheim/qa valheim/tests/Takaro.Valheim.Core.Tests
git commit -m "test(valheim): record owned companion live proof"
```

**Step 5: Create the breaking-change PR and pass CI**

Use the `create-pr` skill with title:

```text
feat(valheim)!: add required owned client companion
```

Include `BREAKING CHANGE:` installation/upgrade text. Run `check-ci`; require conventional-title, ShellCheck, full tests, and both-package job green. Fix failures with red/green tests and repush.

**Step 6: Merge and verify release artifacts**

After green CI and review, squash-merge. Verify release-please opens Valheim `2.0.0`; require its package rehearsal green before merging the release PR. Verify the final GitHub release contains both server and companion ZIPs, inspect both manifests/contents/hashes, and do one final install smoke with the exact released assets before declaring completion.

## Completion Gate

Do not call the companion complete until all of these are simultaneously true:

- Automated tests, setup harness, ShellCheck, real server/client `net472` builds, reproducible dual-package checks, independent review, PR CI, and release CI pass.
- Missing, incompatible, compatible, reconnect, and restart compatibility scenarios pass with exact artifacts.
- Chat, commands/modules, inventory mutation, death, and entity kill persist correctly in Takaro and are visible at the real game boundary.
- Existing server-owned capabilities regressions pass.
- Moderation and shutdown have disposable-state proof and cleanup.
- Client archive contains no credentials/cloud/server code.
- Capability registry and QA ledgers describe server-owned, client-reported, upstream-blocked, and unsupported behavior honestly.
- The final released server and client ZIPs—not merely a development build—pass the installation smoke.
