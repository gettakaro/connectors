using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionChatPolicyTests
{
    [TestMethod]
    public void OrdinaryLocalChatContinuesAndReportsAfterOriginalOnce()
    {
        var policy = new CompanionChatPolicy(new[] { "$" });

        var decision = policy.Evaluate(isLocalPlayer: true, "hello vikings");

        Assert.AreEqual(CompanionChatDisposition.Ordinary, decision.Disposition);
        Assert.IsFalse(decision.ShouldAttemptCommandReport);
        Assert.IsTrue(decision.ShouldReportAfterOriginal);
        Assert.AreEqual("hello vikings", decision.Message);
    }

    [TestMethod]
    public void AcceptedCommandReportsOnceAndSuppressesOriginal()
    {
        var policy = new CompanionChatPolicy(new[] { "$" });

        var decision = policy.Evaluate(isLocalPlayer: true, "$help");
        var hooks = ReadCompanionSource("CompanionClientHooks.cs");

        Assert.AreEqual(CompanionChatDisposition.Command, decision.Disposition);
        Assert.IsTrue(decision.ShouldAttemptCommandReport);
        Assert.IsFalse(decision.ShouldReportAfterOriginal);
        StringAssert.Contains(hooks, "activeBridge.TrySendChat(decision.Message)");
        StringAssert.Contains(hooks, "suppressOriginal: commandAccepted");
        StringAssert.Contains(hooks, "return !__state.SuppressOriginal");
        StringAssert.Contains(hooks, "if (__state.ReportAfterOriginal)");
    }

    [TestMethod]
    public void RemoteBlankAndOversizedChatAreIgnored()
    {
        var policy = new CompanionChatPolicy(new[] { "$" });

        foreach (var decision in new[]
                 {
                     policy.Evaluate(isLocalPlayer: false, "remote"),
                     policy.Evaluate(isLocalPlayer: true, null),
                     policy.Evaluate(isLocalPlayer: true, string.Empty),
                     policy.Evaluate(isLocalPlayer: true, "   "),
                     policy.Evaluate(
                         isLocalPlayer: true,
                         new string('x', CompanionProtocol.MaximumChatCharacters + 1))
                 })
        {
            Assert.AreEqual(CompanionChatDisposition.Ignore, decision.Disposition);
            Assert.IsFalse(decision.ShouldAttemptCommandReport);
            Assert.IsFalse(decision.ShouldReportAfterOriginal);
        }
    }

    [TestMethod]
    public void ConfiguredPrefixMatchingIsOrdinalAndAnchored()
    {
        var prefixes = CompanionChatPolicy.ParsePrefixes(" $ ; /tk ; $ ;  !  ");
        var policy = new CompanionChatPolicy(prefixes);

        CollectionAssert.AreEqual(new[] { "$", "/tk", "!" }, prefixes.ToArray());
        Assert.AreEqual(
            CompanionChatDisposition.Command,
            policy.Evaluate(true, "$help").Disposition);
        Assert.AreEqual(
            CompanionChatDisposition.Command,
            policy.Evaluate(true, "/tk status").Disposition);
        Assert.AreEqual(
            CompanionChatDisposition.Command,
            policy.Evaluate(true, "!players").Disposition);
        Assert.AreEqual(
            CompanionChatDisposition.Ordinary,
            policy.Evaluate(true, " $help").Disposition);
        Assert.AreEqual(
            CompanionChatDisposition.Ordinary,
            policy.Evaluate(true, "/TK status").Disposition);
        Assert.AreEqual(
            CompanionChatDisposition.Ordinary,
            policy.Evaluate(true, "hello $help").Disposition);
    }

    private static string ReadCompanionSource(string fileName) =>
        File.ReadAllText(Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../src/Takaro.Valheim.Companion",
            fileName)));
}
