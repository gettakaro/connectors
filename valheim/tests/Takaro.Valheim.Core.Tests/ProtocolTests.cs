using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ProtocolTests
{
    [TestMethod]
    public void ParseRequestAcceptsArgsAsJsonObject()
    {
        var request = TakaroProtocol.ParseRequest("""
        {
          "type": "request",
          "requestId": "req-1",
          "payload": {
            "action": "sendMessage",
            "args": { "message": "hello vikings" }
          }
        }
        """);

        Assert.AreEqual("req-1", request.RequestId);
        Assert.AreEqual("sendMessage", request.Action);
        Assert.AreEqual("hello vikings", request.Args.GetProperty("message").GetString());
    }

    [TestMethod]
    public void ParseRequestAcceptsArgsAsJsonString()
    {
        var request = TakaroProtocol.ParseRequest("""
        {
          "type": "request",
          "requestId": "req-2",
          "payload": {
            "action": "kickPlayer",
            "args": "{\"gameId\":\"Steam_76561198000000001\",\"reason\":\"spam\"}"
          }
        }
        """);

        Assert.AreEqual("Steam_76561198000000001", request.Args.GetProperty("gameId").GetString());
        Assert.AreEqual("spam", request.Args.GetProperty("reason").GetString());
    }

    [TestMethod]
    public void CreateResponseAndEventUseTakaroEnvelope()
    {
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse("req-3", new { ok = true })).RootElement;
        Assert.AreEqual("response", response.GetProperty("type").GetString());
        Assert.AreEqual("req-3", response.GetProperty("requestId").GetString());
        Assert.IsTrue(response.GetProperty("payload").GetProperty("ok").GetBoolean());

        var gameEvent = JsonDocument.Parse(TakaroProtocol.CreateGameEvent("chat-message", new { msg = "hello" })).RootElement;
        Assert.AreEqual("gameEvent", gameEvent.GetProperty("type").GetString());
        Assert.AreEqual("chat-message", gameEvent.GetProperty("payload").GetProperty("type").GetString());
        Assert.AreEqual("hello", gameEvent.GetProperty("payload").GetProperty("data").GetProperty("msg").GetString());
    }

    [TestMethod]
    public void CreateResponseUnwrapsSuccessfulActionResultPayload()
    {
        var response = JsonDocument.Parse(
            TakaroProtocol.CreateResponse("req-4", TakaroActionResult.Ok(new { connectable = true }))).RootElement;

        var payload = response.GetProperty("payload");
        Assert.IsTrue(payload.GetProperty("connectable").GetBoolean());
        Assert.IsFalse(payload.TryGetProperty("success", out _));
    }

    [TestMethod]
    public void CreateActionResponseKeepsSuccessMetadataOutsideSchemaPayload()
    {
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse(
            "req-success",
            "getPlayerLocation",
            TakaroActionResult.Ok(new TakaroPosition(12, 34, 56, "valheim")))).RootElement;

        Assert.IsTrue(response.GetProperty("success").GetBoolean());
        Assert.AreEqual(12, response.GetProperty("payload").GetProperty("x").GetInt32());
        Assert.AreEqual(JsonValueKind.Null, response.GetProperty("errorCode").ValueKind);
    }

    [TestMethod]
    public void CreateActionResponseUsesArrayFallbackForUnavailableInventory()
    {
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse(
            "req-inventory",
            "getPlayerInventory",
            TakaroActionResult.Error("player_component_unavailable", "Remote inventory is client-owned."))).RootElement;

        Assert.IsFalse(response.GetProperty("success").GetBoolean());
        Assert.AreEqual("player_component_unavailable", response.GetProperty("errorCode").GetString());
        Assert.AreEqual(JsonValueKind.Array, response.GetProperty("payload").ValueKind);
        Assert.AreEqual(0, response.GetProperty("payload").GetArrayLength());
    }

    [TestMethod]
    public void CreateActionResponseKeepsSuccessfulInventoryAsArray()
    {
        var items = new[] { new TakaroInventoryItem("Wood", "Wood", 1, "1") };
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse(
            "req-inventory-success",
            "getPlayerInventory",
            TakaroActionResult.Ok(items))).RootElement;

        Assert.IsTrue(response.GetProperty("success").GetBoolean());
        Assert.AreEqual(JsonValueKind.Array, response.GetProperty("payload").ValueKind);
        Assert.AreEqual("Wood", response.GetProperty("payload")[0].GetProperty("code").GetString());
    }

    [DataTestMethod]
    [DataRow("player_position_unavailable")]
    [DataRow("player_not_found")]
    public void CreateActionResponseUsesTypedPositionFallbackForUnavailableLocation(string errorCode)
    {
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse(
            "req-location",
            "getPlayerLocation",
            TakaroActionResult.Error(errorCode, "No current server-owned position."))).RootElement;

        Assert.IsFalse(response.GetProperty("success").GetBoolean());
        Assert.AreEqual(errorCode, response.GetProperty("errorCode").GetString());
        var payload = response.GetProperty("payload");
        Assert.AreEqual(0, payload.GetProperty("x").GetDouble());
        Assert.AreEqual(0, payload.GetProperty("y").GetDouble());
        Assert.AreEqual(0, payload.GetProperty("z").GetDouble());
        Assert.AreEqual("unavailable", payload.GetProperty("dimension").GetString());
    }

    [TestMethod]
    public void CreateIdentifyUsesKnownConnectorPayloadShape()
    {
        var config = new ConnectorConfig(
            RegistrationToken: "reg-token",
            ServerName: "Valheim Test",
            IdentityToken: "Valheim Test",
            TakaroWsUrl: "wss://connect.takaro.io/",
            LogLevel: "Information",
            EnableLogEvents: false,
            CommandAllowlistExact: new[] { "help" },
            CommandAllowlistPrefixes: Array.Empty<string>());

        var identify = JsonDocument.Parse(TakaroProtocol.CreateIdentify(config)).RootElement;
        var payload = identify.GetProperty("payload");

        Assert.AreEqual("identify", identify.GetProperty("type").GetString());
        Assert.AreEqual("Valheim Test", payload.GetProperty("identityToken").GetString());
        Assert.AreEqual("reg-token", payload.GetProperty("registrationToken").GetString());
        Assert.AreEqual("Valheim Test", payload.GetProperty("name").GetString());
        Assert.IsFalse(payload.TryGetProperty("serverName", out _));
    }
}
