using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class ProtocolTests
{
    [TestMethod]
    public void CreateIdentifyUsesTakaroNameAndStableIdentity()
    {
        var config = new ConnectorConfig
        {
            TakaroWsUrl = "wss://connect.takaro.io/",
            ServerName = "Dust II",
            IdentityToken = "cs2-dust2-stable",
            RegistrationToken = "registration-token"
        };

        var message = JsonDocument.Parse(TakaroProtocol.CreateIdentify(config)).RootElement;

        Assert.AreEqual("identify", message.GetProperty("type").GetString());
        var payload = message.GetProperty("payload");
        Assert.AreEqual("Dust II", payload.GetProperty("name").GetString());
        Assert.AreEqual("cs2-dust2-stable", payload.GetProperty("identityToken").GetString());
        Assert.AreEqual("registration-token", payload.GetProperty("registrationToken").GetString());
        Assert.IsFalse(payload.TryGetProperty("serverName", out _));
    }

    [TestMethod]
    public void ParseRequestAcceptsArgsAsJsonString()
    {
        var request = TakaroProtocol.ParseRequest("""
        {
          "type": "request",
          "requestId": "req-1",
          "payload": {
            "action": "sendMessage",
            "args": "{\"message\":\"hello counter-strike\"}"
          }
        }
        """);

        Assert.AreEqual("req-1", request.RequestId);
        Assert.AreEqual("sendMessage", request.Action);
        Assert.AreEqual("hello counter-strike", request.Args.GetProperty("message").GetString());
    }

    [TestMethod]
    public void CreateResponseAndGameEventUseTakaroEnvelope()
    {
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse("req-2", new { success = true })).RootElement;

        Assert.AreEqual("response", response.GetProperty("type").GetString());
        Assert.AreEqual("req-2", response.GetProperty("requestId").GetString());
        Assert.IsTrue(response.GetProperty("payload").GetProperty("success").GetBoolean());

        var gameEvent = JsonDocument.Parse(TakaroProtocol.CreateGameEvent("chat-message", new { msg = "rush b" })).RootElement;
        Assert.AreEqual("gameEvent", gameEvent.GetProperty("type").GetString());
        Assert.AreEqual("chat-message", gameEvent.GetProperty("payload").GetProperty("type").GetString());
        Assert.AreEqual("rush b", gameEvent.GetProperty("payload").GetProperty("data").GetProperty("msg").GetString());
    }

    [TestMethod]
    public void CreateResponseUnwrapsSuccessfulActionResultPayload()
    {
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse("req-3", TakaroActionResult.Ok(new { reachable = true }))).RootElement;

        var payload = response.GetProperty("payload");
        Assert.IsTrue(payload.GetProperty("reachable").GetBoolean());
        Assert.IsFalse(payload.TryGetProperty("success", out _));
        Assert.IsFalse(payload.TryGetProperty("error", out _));
    }

    [TestMethod]
    public void CreateResponseKeepsExplicitNullReason()
    {
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse("req-reachability",
            TakaroActionResult.Ok(new { connectable = true, reason = (string?)null }))).RootElement;

        var payload = response.GetProperty("payload");
        Assert.IsTrue(payload.GetProperty("connectable").GetBoolean());
        Assert.AreEqual(JsonValueKind.Null, payload.GetProperty("reason").ValueKind);
    }

    [TestMethod]
    public void CreateResponseKeepsExplicitErrorForFailedActionResult()
    {
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse("req-4", TakaroActionResult.Fail("blocked_command", "nope"))).RootElement;

        var payload = response.GetProperty("payload");
        Assert.AreEqual("blocked_command: nope", payload.GetProperty("error").GetString());
    }

    [TestMethod]
    public void FailedActionsRetainTakaroValidatedResponseShapes()
    {
        var result = TakaroActionResult.Fail("offline", "Player is not spawned.");
        var location = JsonDocument.Parse(TakaroProtocol.CreateActionResponse("req-loc", "getPlayerLocation", result)).RootElement.GetProperty("payload");
        Assert.AreEqual(0d, location.GetProperty("x").GetDouble());
        Assert.AreEqual("offline: Player is not spawned.", location.GetProperty("error").GetString());

        var reachability = JsonDocument.Parse(TakaroProtocol.CreateActionResponse("req-reach", "testReachability", result)).RootElement.GetProperty("payload");
        Assert.IsFalse(reachability.GetProperty("connectable").GetBoolean());
        Assert.AreEqual("offline: Player is not spawned.", reachability.GetProperty("reason").GetString());
    }
}
