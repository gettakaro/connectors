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

        var message = JsonDocument.Parse(TakaroProtocol.CreateIdentify(config, [new { name = "players.list" }])).RootElement;

        Assert.AreEqual("identify", message.GetProperty("type").GetString());
        var payload = message.GetProperty("payload");
        Assert.AreEqual("Dust II", payload.GetProperty("name").GetString());
        Assert.AreEqual("cs2-dust2-stable", payload.GetProperty("identityToken").GetString());
        Assert.AreEqual("registration-token", payload.GetProperty("registrationToken").GetString());
        Assert.IsFalse(payload.TryGetProperty("serverName", out _));
        Assert.AreEqual("counter-strike2", payload.GetProperty("game").GetString());
        Assert.AreEqual("takaro-counter-strike2", payload.GetProperty("connector").GetProperty("name").GetString());
        Assert.AreEqual(1, payload.GetProperty("protocolVersions")[0].GetInt32());
        Assert.AreEqual("players.list", payload.GetProperty("capabilities")[0].GetProperty("name").GetString());
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

        var gameEvent = JsonDocument.Parse(TakaroProtocol.CreateGameEvent("chat-message", new { msg = "rush b" }, "stream-1", 7)).RootElement;
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
    public void GameEventsCarryStreamAndSequence()
    {
        var payload = JsonDocument.Parse(TakaroProtocol.CreateGameEvent("log", new { msg = "x" }, "stream-1", 7)).RootElement.GetProperty("payload");
        Assert.AreEqual("stream-1", payload.GetProperty("streamId").GetString());
        Assert.AreEqual(7, payload.GetProperty("seq").GetInt64());
    }

    [TestMethod]
    public void FailedActionsUseVersionOneErrorsAndKeepNullAnswers()
    {
        var missing = TakaroActionResult.Fail("player_not_found", "Gone.");
        var location = JsonDocument.Parse(TakaroProtocol.CreateActionResponse("req-loc", "getPlayerLocation", missing)).RootElement;
        Assert.AreEqual(JsonValueKind.Null, location.GetProperty("payload").ValueKind);
        Assert.IsFalse(location.TryGetProperty("error", out _));

        var kick = JsonDocument.Parse(TakaroProtocol.CreateActionResponse("req-kick", "kickPlayer", missing)).RootElement;
        Assert.AreEqual("response", kick.GetProperty("type").GetString());
        Assert.AreEqual("not_found", kick.GetProperty("error").GetProperty("code").GetString());

        var offline = TakaroActionResult.Fail("offline", "Player is not spawned.");
        var reachability = JsonDocument.Parse(TakaroProtocol.CreateActionResponse("req-reach", "testReachability", offline)).RootElement.GetProperty("payload");
        Assert.IsFalse(reachability.GetProperty("connectable").GetBoolean());
        Assert.AreEqual("offline: Player is not spawned.", reachability.GetProperty("reason").GetString());

        var refused = JsonDocument.Parse(TakaroProtocol.CreateActionResponse("req-cmd", "executeConsoleCommand", offline)).RootElement.GetProperty("payload");
        Assert.IsFalse(refused.GetProperty("success").GetBoolean());
    }

    [TestMethod]
    [DataRow("invalid_amount", "invalid_args")]
    [DataRow("unknown_item", "invalid_args")]
    [DataRow("unsupported_action", "unsupported")]
    [DataRow("ban_not_found", "not_found")]
    [DataRow("tile_out_of_range", "not_found")]
    [DataRow("action_failed", "internal")]
    [DataRow("delivery_failed", "game_error")]
    public void InternalCodesMapToTheFixedProtocolSet(string code, string expected) =>
        Assert.AreEqual(expected, TakaroProtocol.ToProtocolErrorCode(code));
}
