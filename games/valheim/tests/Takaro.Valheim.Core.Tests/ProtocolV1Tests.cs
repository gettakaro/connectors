using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

/// <summary>Generic Game Protocol v1 handshake, manifest, errors and event stream; v0 stays as it was.</summary>
[TestClass]
public sealed class ProtocolV1Tests
{
    [TestMethod]
    public void IdentifyOffersBothVersionsWithTheManifest()
    {
        var identify = JsonDocument.Parse(TakaroProtocol.CreateIdentify(Config())).RootElement;
        var payload = identify.GetProperty("payload");

        Assert.AreEqual("identify", identify.GetProperty("type").GetString());
        Assert.AreEqual("token", payload.GetProperty("registrationToken").GetString());
        Assert.AreEqual("identity", payload.GetProperty("identityToken").GetString());
        CollectionAssert.AreEqual(new[] { 0, 1 }, payload.GetProperty("protocolVersions").EnumerateArray().Select(v => v.GetInt32()).ToArray());
        Assert.AreEqual(JsonValueKind.Array, payload.GetProperty("capabilities").ValueKind);
    }

    [TestMethod]
    public void ManifestDeclaresExactlyTheLiveSupportedActionsAndEveryEvent()
    {
        using var registry = JsonDocument.Parse(ReadValheimFile("capabilities.json"));
        var liveActions = registry.RootElement.GetProperty("actions").EnumerateObject()
            .Where(a => a.Value.GetString() == "live-supported"
                && a.Name is not (TakaroActionNames.TestReachability or TakaroActionNames.GetGameIcon))
            .Select(a => a.Name)
            .OrderBy(a => a, StringComparer.Ordinal)
            .ToArray();
        CollectionAssert.AreEqual(
            liveActions,
            ProtocolHandshake.ActionCapabilities.Keys.OrderBy(a => a, StringComparer.Ordinal).ToArray());

        var names = ProtocolHandshake.BuildCapabilityManifest().Select(c => c!["name"]!.GetValue<string>()).ToArray();
        CollectionAssert.AllItemsAreUnique(names);
        foreach (var eventName in GameEventTypes.All)
        {
            CollectionAssert.Contains(names, "events." + eventName);
        }

        CollectionAssert.DoesNotContain(names, "players.get", "getPlayer is unsupported");
        CollectionAssert.DoesNotContain(names, "map", "declared only when the runner has the world map");
        CollectionAssert.Contains(
            ProtocolHandshake.BuildCapabilityManifest(includeMap: true).Select(c => c!["name"]!.GetValue<string>()).ToArray(),
            "map");
        foreach (var name in names)
        {
            StringAssert.Matches(name, new System.Text.RegularExpressions.Regex(@"^[a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9-]*)*$"));
        }
    }

    [TestMethod]
    public void UndeclaredActionsAreRefusedOnVersionOneOnly()
    {
        Assert.IsTrue(ProtocolHandshake.IsDeclared(TakaroActionNames.TestReachability));
        Assert.IsTrue(ProtocolHandshake.IsDeclared(TakaroActionNames.SendMessage));
        Assert.IsFalse(ProtocolHandshake.IsDeclared(TakaroActionNames.GetPlayer));
        Assert.IsFalse(ProtocolHandshake.IsDeclared(TakaroActionNames.GetMapInfo));
        Assert.IsFalse(ProtocolHandshake.IsDeclared(TakaroActionNames.GetMapInfo, mapDeclared: true), "version 1 refuses getMapInfo");
        Assert.IsTrue(ProtocolHandshake.IsDeclared(TakaroActionNames.GetMapTile, mapDeclared: true));
        Assert.IsFalse(ProtocolHandshake.IsDeclared("blowUpTheWorld"));
    }

    [TestMethod]
    public void IdentifyResponseWithoutVersionIsVersionZero()
    {
        var reply = ProtocolHandshake.ParseIdentifyResponse("""{"type":"identifyResponse","payload":{"gameServerId":"gs-1"}}""");

        Assert.IsTrue(reply.Accepted);
        Assert.AreEqual("gs-1", reply.GameServerId);
        Assert.AreEqual(0, reply.ProtocolVersion);
    }

    [TestMethod]
    [DataRow(0)]
    [DataRow(1)]
    public void IdentifyResponseNamesTheChosenVersion(int version)
    {
        var reply = ProtocolHandshake.ParseIdentifyResponse(
            "{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs-1\",\"protocolVersion\":" + version + "},\"requestId\":\"r\"}");

        Assert.IsTrue(reply.Accepted);
        Assert.AreEqual(version, reply.ProtocolVersion);
    }

    [TestMethod]
    public void UnknownChosenVersionIsARejection()
    {
        var reply = ProtocolHandshake.ParseIdentifyResponse("""{"type":"identifyResponse","payload":{"gameServerId":"gs-1","protocolVersion":7}}""");

        Assert.IsFalse(reply.Accepted);
        Assert.AreEqual(ProtocolErrorCodes.NoSharedProtocolVersion, reply.ErrorCode);
    }

    [TestMethod]
    public void VersionOneRejectionCarriesItsCode()
    {
        var reply = ProtocolHandshake.ParseIdentifyResponse("""
        {"type":"identifyResponse","payload":null,
         "error":{"code":"invalid_args","message":"invalid capability name: \"Players\""},"requestId":"r"}
        """);

        Assert.IsFalse(reply.Accepted);
        Assert.AreEqual(ProtocolErrorCodes.InvalidArgs, reply.ErrorCode);
        StringAssert.Contains(reply.Reason, "invalid capability name");
    }

    [TestMethod]
    public void VersionZeroRejectionHasNoCode()
    {
        var reply = ProtocolHandshake.ParseIdentifyResponse("""{"type":"identifyResponse","payload":{"error":{"message":"Invalid registrationToken provided"}}}""");

        Assert.IsFalse(reply.Accepted);
        Assert.IsNull(reply.ErrorCode);
        StringAssert.Contains(reply.Reason, "Invalid registrationToken provided");
    }

    [TestMethod]
    public void StreamPositionGoesNextToTypeAndDataAndKeepsTheData()
    {
        var frame = TakaroProtocol.CreateGameEvent("chat-message", new { msg = "hi", timestamp = "2026-10-19T12:00:00.000Z" });

        var positioned = JsonDocument.Parse(ProtocolHandshake.WithStreamPosition(frame, "stream-1", 42)).RootElement.GetProperty("payload");

        Assert.AreEqual("chat-message", positioned.GetProperty("type").GetString());
        Assert.AreEqual("stream-1", positioned.GetProperty("streamId").GetString());
        Assert.AreEqual(42, positioned.GetProperty("seq").GetInt64());
        Assert.AreEqual("2026-10-19T12:00:00.000Z", positioned.GetProperty("data").GetProperty("timestamp").GetString());
        Assert.IsFalse(positioned.GetProperty("data").TryGetProperty("seq", out _));
    }

    [TestMethod]
    public void EventAckAndEventErrorAreRead()
    {
        Assert.IsTrue(ProtocolHandshake.TryParseEventAck(
            """{"type":"eventAck","payload":{"streamId":"s","upTo":9},"requestId":"r"}""", out var stream, out var upTo));
        Assert.AreEqual("s", stream);
        Assert.AreEqual(9L, upTo);
        Assert.IsFalse(ProtocolHandshake.TryParseEventAck("""{"type":"eventAck","payload":{"upTo":9}}""", out _, out _));

        var error = ProtocolHandshake.DescribeEventError(
            """{"type":"eventError","payload":{"streamId":"s","seq":4},"error":{"code":"invalid_args","message":"data failed validation"}}""");
        StringAssert.Contains(error, "seq 4");
        StringAssert.Contains(error, "data failed validation");
    }

    [TestMethod]
    [DataRow("invalid_args", "invalid_args")]
    [DataRow("invalid_quality", "invalid_args")]
    [DataRow("player_not_found", "not_found")]
    [DataRow("item_not_found", "not_found")]
    [DataRow("unsupported_action", "unsupported")]
    [DataRow("server_only_unsupported", "unsupported")]
    [DataRow("action_failed", "internal")]
    [DataRow("znet_unavailable", "game_error")]
    [DataRow("inventory_snapshot_stale", "game_error")]
    public void ConnectorErrorCodesMapOntoTheVersionOneSet(string connectorCode, string protocolCode)
    {
        Assert.AreEqual(protocolCode, ProtocolHandshake.ToProtocolErrorCode(connectorCode));
    }

    [TestMethod]
    public void VersionOneFailuresUseTheStandardErrorShapeEvenForArrayActions()
    {
        Assert.IsTrue(TakaroProtocol.TryCreateActionResponse(
            "req-1",
            TakaroActionNames.GetPlayerInventory,
            TakaroActionResult.Error("server_only_unsupported", "needs the optional mod"),
            out var frame,
            protocolVersion: 1));

        var root = JsonDocument.Parse(frame!).RootElement;
        Assert.AreEqual("response", root.GetProperty("type").GetString());
        Assert.AreEqual("req-1", root.GetProperty("requestId").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("error").GetProperty("code").GetString());
        StringAssert.Contains(root.GetProperty("error").GetProperty("message").GetString(), "needs the optional mod");
        Assert.IsFalse(root.TryGetProperty("payload", out _));

        Assert.IsFalse(TakaroProtocol.TryCreateActionResponse(
            "req-2",
            TakaroActionNames.GetPlayerInventory,
            TakaroActionResult.Error("server_only_unsupported", "needs the optional mod"),
            out _),
            "version 0 still suppresses array failures");
    }

    [TestMethod]
    public void VersionOneUnreachableIsAnAnswerNotAnError()
    {
        TakaroProtocol.TryCreateActionResponse(
            "req-1",
            TakaroActionNames.TestReachability,
            TakaroActionResult.Error("runtime_unavailable", "world not loaded"),
            out var frame,
            protocolVersion: 1);

        var root = JsonDocument.Parse(frame!).RootElement;
        Assert.IsFalse(root.TryGetProperty("error", out _));
        Assert.IsFalse(root.GetProperty("payload").GetProperty("connectable").GetBoolean());
        StringAssert.Contains(root.GetProperty("payload").GetProperty("reason").GetString(), "world not loaded");
    }

    [TestMethod]
    public void VersionOneLocationOfAPlayerWhoIsNotOnlineIsNull()
    {
        TakaroProtocol.TryCreateActionResponse(
            "req-1",
            TakaroActionNames.GetPlayerLocation,
            TakaroActionResult.Error("player_not_found", "No online Valheim player matched Steam_1."),
            out var frame,
            protocolVersion: 1);

        var root = JsonDocument.Parse(frame!).RootElement;
        Assert.AreEqual(JsonValueKind.Null, root.GetProperty("payload").ValueKind);
        Assert.IsFalse(root.TryGetProperty("error", out _));

        TakaroProtocol.TryCreateActionResponse(
            "req-2",
            TakaroActionNames.GetPlayerLocation,
            TakaroActionResult.Error("runtime_unavailable", "no observed position"),
            out frame,
            protocolVersion: 1);
        Assert.AreEqual("game_error", JsonDocument.Parse(frame!).RootElement.GetProperty("error").GetProperty("code").GetString());
    }

    [TestMethod]
    public void StreamPositionsAreAssignedOnEnqueueAndSurviveAReconnect()
    {
        var queue = new PendingEventQueue(capacity: 10);
        queue.Enqueue("one");
        queue.Enqueue("two");

        Assert.IsTrue(queue.TryPeekUnsent(out var frame, out var seq));
        Assert.AreEqual(1L, seq);
        queue.MarkSent(frame);
        queue.ResetForNewConnection();

        Assert.IsTrue(queue.TryPeekUnsent(out frame, out seq));
        Assert.AreEqual("one", frame);
        Assert.AreEqual(1L, seq, "a resent event keeps its seq so Takaro dedupes it");
        queue.MarkSent(frame);
        Assert.IsTrue(queue.TryPeekUnsent(out _, out seq));
        Assert.AreEqual(2L, seq);
    }

    [TestMethod]
    public void OnlyEventAckReleasesFramesOnVersionOne()
    {
        var queue = new PendingEventQueue(capacity: 10);
        queue.Enqueue("one");
        queue.Enqueue("two");
        queue.TryPeekUnsent(out var frame);
        queue.MarkSent(frame);
        queue.TryPeekUnsent(out frame);
        queue.MarkSent(frame);
        queue.AddCheckpoint();

        Assert.AreEqual(0, queue.ConfirmOldestCheckpoint(releaseFrames: false));
        Assert.AreEqual(0, queue.OutstandingCheckpoints, "the pong still answers its ping");
        Assert.AreEqual(2, queue.Count);

        Assert.AreEqual(1, queue.AcknowledgeUpTo(1));
        Assert.AreEqual(1, queue.Count);
        Assert.AreEqual(0, queue.AcknowledgeUpTo(1), "a repeated ack changes nothing");
        Assert.AreEqual(1, queue.AcknowledgeUpTo(5));
        Assert.AreEqual(0, queue.Count);
    }

    private static ConnectorConfig Config() =>
        ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "token",
            ["identityToken"] = "identity",
            ["serverName"] = "Valheim Test"
        });

    private static string ReadValheimFile(string relativePath) =>
        File.ReadAllText(Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../", relativePath)));
}
