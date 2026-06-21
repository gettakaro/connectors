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
