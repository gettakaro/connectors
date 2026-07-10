using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class TakaroConsumerContractTests
{
    [TestMethod]
    public void TakaroResolvesOnlyPayloadAndIgnoresRootFailureMetadata()
    {
        const string oldInventoryFallback = """
        {
          "type": "response",
          "requestId": "inventory",
          "payload": [],
          "success": false,
          "errorCode": "player_component_unavailable",
          "message": "Remote inventory is unavailable."
        }
        """;

        var resolved = ResolveLikeCurrentTakaro(oldInventoryFallback);

        Assert.AreEqual(JsonValueKind.Array, resolved.ValueKind);
        Assert.AreEqual(0, resolved.GetArrayLength());
    }

    [TestMethod]
    public void UnsupportedInventoryProducesNoFrameForTakaroToResolve()
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            "inventory",
            "getPlayerInventory",
            TakaroActionResult.Error("player_component_unavailable", "Dedicated servers do not expose remote inventory."),
            out var frame);

        Assert.IsFalse(shouldSend);
        Assert.IsNull(frame);
    }

    [DataTestMethod]
    [DataRow("player_position_unavailable")]
    [DataRow("player_not_found")]
    public void UnavailableLocationUsesSchemaValidPayloadErrorThatTakaroRejects(string errorCode)
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            "location",
            "getPlayerLocation",
            TakaroActionResult.Error(errorCode, "No real server-observed position is available."),
            out var frame);

        Assert.IsTrue(shouldSend);
        Assert.IsNotNull(frame);
        var exception = Assert.ThrowsException<InvalidOperationException>(() => ResolveLikeCurrentTakaro(frame!));
        StringAssert.Contains(exception.Message, errorCode);

        using var document = JsonDocument.Parse(frame!);
        var payload = document.RootElement.GetProperty("payload");
        Assert.AreEqual(0, payload.GetProperty("x").GetDouble());
        Assert.AreEqual(0, payload.GetProperty("y").GetDouble());
        Assert.AreEqual(0, payload.GetProperty("z").GetDouble());
        Assert.IsTrue(payload.TryGetProperty("error", out _));
        Assert.IsFalse(document.RootElement.TryGetProperty("success", out _));
        Assert.IsFalse(document.RootElement.TryGetProperty("errorCode", out _));
    }

    [TestMethod]
    public void RealLocationProducesExactlyOneResolvablePayload()
    {
        var shouldSend = TakaroProtocol.TryCreateActionResponse(
            "location",
            "getPlayerLocation",
            TakaroActionResult.Ok(new TakaroPosition(12, 34, 56, "valheim")),
            out var frame);

        Assert.IsTrue(shouldSend);
        Assert.IsNotNull(frame);
        var resolved = ResolveLikeCurrentTakaro(frame!);
        Assert.AreEqual(12, resolved.GetProperty("x").GetDouble());
        Assert.AreEqual(34, resolved.GetProperty("y").GetDouble());
        Assert.AreEqual(56, resolved.GetProperty("z").GetDouble());
        Assert.IsFalse(resolved.TryGetProperty("error", out _));
    }

    [TestMethod]
    public void SuppressedResponseLogsAreRateLimitedPerFailure()
    {
        var limiter = new SuppressedResponseLogLimiter(TimeSpan.FromSeconds(30));
        var now = DateTimeOffset.Parse("2026-07-10T15:30:00+02:00");

        Assert.IsTrue(limiter.ShouldLog("getPlayerInventory", "player_component_unavailable", now));
        Assert.IsFalse(limiter.ShouldLog("getPlayerInventory", "player_component_unavailable", now.AddSeconds(29)));
        Assert.IsTrue(limiter.ShouldLog("giveItem", "invalid_amount", now.AddSeconds(1)));
        Assert.IsTrue(limiter.ShouldLog("getPlayerInventory", "player_component_unavailable", now.AddSeconds(30)));
    }

    private static JsonElement ResolveLikeCurrentTakaro(string responseFrame)
    {
        using var document = JsonDocument.Parse(responseFrame);
        var payload = document.RootElement.GetProperty("payload");
        if (payload.ValueKind == JsonValueKind.Object
            && payload.TryGetProperty("error", out var error)
            && error.ValueKind == JsonValueKind.String)
        {
            throw new InvalidOperationException(error.GetString());
        }

        return payload.Clone();
    }
}
