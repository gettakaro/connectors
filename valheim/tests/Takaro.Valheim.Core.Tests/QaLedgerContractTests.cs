using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class QaLedgerContractTests
{
    [TestMethod]
    public void LedgerRecordsServerCorePassWithoutClaimingLivePlayerProof()
    {
        var ledger = ReadLedger();

        StringAssert.Contains(ledger, "BUILD/SERVER-CORE PASS; LIVE-PLAYER BLOCKED");
        Assert.IsFalse(ledger.Contains("PASS WITH GAPS", StringComparison.Ordinal));
        StringAssert.Contains(ledger, "testReachability");
        StringAssert.Contains(ledger, "executeConsoleCommand `help`");
        StringAssert.Contains(ledger, "listBans");
        StringAssert.Contains(ledger, "teleports`, `Waypoints`, and `serverMessages");
        StringAssert.Contains(ledger, "server message routed to 0 peer(s)");
    }

    [TestMethod]
    public void LedgerRecordsTheExactVanillaClientAutomationBlocker()
    {
        var ledger = ReadLedger();

        StringAssert.Contains(ledger, "fixed click `(610,598)` did not transition from the main menu");
        StringAssert.Contains(ledger, "No player data");
        StringAssert.Contains(ledger, "characters/hehe.fch");
        StringAssert.Contains(ledger, "no handshake, character ZDO, player-connected event, or nonempty getPlayers result");
        StringAssert.Contains(ledger, "UI automation/profile state outside this connector branch");
    }

    [TestMethod]
    public void LedgerKeepsPlayerBoundAndDestructiveChecksExplicitlyBlockedOrSkipped()
    {
        var ledger = ReadLedger();

        foreach (var marker in new[]
                 {
                     "getPlayerLocation",
                     "giveItem",
                     "teleportPlayer",
                     "player-visible messaging",
                     "lifecycle/death",
                     "listItems/listEntities/listLocations",
                     "kickPlayer`, `banPlayer`, `unbanPlayer`, and `shutdown` were skipped"
                 })
        {
            StringAssert.Contains(ledger, marker);
        }
    }

    private static string ReadLedger()
    {
        var path = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../qa/2026-07-10-server-only-validation.md"));
        return File.ReadAllText(path);
    }
}
