using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class BanTermsTests
{
    private static readonly DateTimeOffset Now = new(2026, 11, 23, 12, 0, 0, TimeSpan.Zero);

    [TestMethod]
    public void TimedBanIsDueOnlyOnceItsEndHasPassed()
    {
        var terms = new BanTerms();
        terms.Record("Steam_76561198000735875", "griefing", Now.AddMinutes(5));
        terms.Record("Steam_76561198000000002", "spam", null);

        CollectionAssert.AreEqual(Array.Empty<string>(), terms.Due(Now).ToArray());
        CollectionAssert.AreEqual(new[] { "Steam_76561198000735875" }, terms.Due(Now.AddMinutes(5)).ToArray());
        CollectionAssert.AreEqual(new[] { "Steam_76561198000735875" }, terms.Due(Now.AddDays(400)).ToArray());
    }

    [TestMethod]
    public void TermsSurviveARestartAndAcceptEveryIdForm()
    {
        var path = Path.Combine(Path.GetTempPath(), $"ban-terms-{Guid.NewGuid():N}.json");
        try
        {
            new BanTerms(path).Record("76561198000735875", "  griefing ", Now.AddHours(1));

            var reloaded = new BanTerms(path);
            reloaded.Load();

            Assert.IsTrue(reloaded.TryGet("Steam_76561198000735875", out var term));
            Assert.AreEqual("griefing", term.Reason);
            Assert.AreEqual(Now.AddHours(1), term.ExpiresAt);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [TestMethod]
    public void UnbanAndInGameLiftDropTheTerms()
    {
        var terms = new BanTerms();
        terms.Record("Steam_1", "a", Now.AddHours(1));
        terms.Record("Steam_2", "b", Now.AddHours(1));
        terms.Record("Steam_3", "c", null);

        Assert.AreEqual(1, terms.Forget(new[] { "Steam_1", "Steam_9" }));
        Assert.AreEqual(1, terms.Retain(new[] { "Steam_3" }));

        Assert.IsFalse(terms.TryGet("Steam_1", out _));
        Assert.IsFalse(terms.TryGet("Steam_2", out _));
        Assert.IsTrue(terms.TryGet("Steam_3", out _));
    }

    [TestMethod]
    public void ListBansReportsStoredReasonAndExpiry()
    {
        var entries = ModerationFactory.CreateBanEntries(new[]
        {
            new ValheimBan("Steam_76561198000735875", "Hehe", Reason: "griefing", ExpiresAt: Now.AddHours(1)),
            new ValheimBan("Steam_76561198000000002", "Odin")
        });

        var response = TakaroProtocol.CreateResponse("list-bans", TakaroActionResult.Ok(entries));
        using var document = JsonDocument.Parse(response);
        var payload = document.RootElement.GetProperty("payload");

        Assert.AreEqual("griefing", payload[0].GetProperty("reason").GetString());
        Assert.AreEqual(Now.AddHours(1), DateTimeOffset.Parse(payload[0].GetProperty("expiresAt").GetString()!));
        Assert.AreEqual("", payload[1].GetProperty("reason").GetString());
        Assert.AreEqual(JsonValueKind.Null, payload[1].GetProperty("expiresAt").ValueKind);
    }
}
