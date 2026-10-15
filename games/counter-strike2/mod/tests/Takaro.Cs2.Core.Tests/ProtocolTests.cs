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
    public void CreateResponseKeepsStructuredFailureForFailedActionResult()
    {
        var response = JsonDocument.Parse(TakaroProtocol.CreateResponse("req-4", TakaroActionResult.Fail("blocked_command", "nope"))).RootElement;

        var payload = response.GetProperty("payload");
        Assert.IsFalse(payload.GetProperty("success").GetBoolean());
        Assert.AreEqual("blocked_command", payload.GetProperty("errorCode").GetString());
        Assert.AreEqual("nope", payload.GetProperty("message").GetString());
    }
}
