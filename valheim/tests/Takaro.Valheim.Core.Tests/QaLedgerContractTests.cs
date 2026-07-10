using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class QaLedgerContractTests
{
    [TestMethod]
    public void LedgerUsesRequiredVerdictAndPinsTurnTwoArtifactEvidence()
    {
        var ledger = ReadLedger();

        StringAssert.Contains(ledger, "## Verdict: PASS WITH GAPS");
        StringAssert.Contains(ledger, "20b505b2fcc5e58a6bdb0ec3bf4d26bda6a5f096");
        StringAssert.Contains(ledger, "4142c2399a660bbda32200e1e18e79e75bb1d3f5b478cf8387681b9a80c1d1ac");
        StringAssert.Contains(ledger, "0a70626f6908669846b8bbfc2d2aa93e44a5902dccc44fba259bb6d0f5c505cc");
        StringAssert.Contains(ledger, "Turn-3 source is not live-validated by this ledger");
    }

    [TestMethod]
    public void LedgerRecordsExactPlayerActionAndCatalogEvidence()
    {
        var ledger = ReadLedger();

        foreach (var marker in new[]
                 {
                     "vanilla `isModded:false`",
                     "14:35:34",
                     "14:40:14",
                     "14:40:26",
                     "`getPlayers`: `0 -> 1 -> 0`",
                     "Wood x1",
                     "amount `1001`",
                     "`135,33,-2 -> 140,33,-2`",
                     "`listItems`: 821",
                     "`listEntities`: 101",
                     "`listLocations`: BLOCKED",
                     "14:51:25"
                 })
        {
            StringAssert.Contains(ledger, marker);
        }
    }

    [TestMethod]
    public void LedgerSeparatesTransportAttemptsFromTakaroPersistenceAndModuleProof()
    {
        var ledger = ReadLedger();

        foreach (var marker in new[]
                 {
                     "eventSearch",
                     "zero persisted lifecycle events",
                     "two `serverMessages` cron deliveries",
                     "cronjob-executed",
                     "commandTrigger",
                     "404",
                     "No hooks were installed",
                     "Destructive actions",
                     "/tmp/valheim-turn2-visible-direct-window.png",
                     "/tmp/valheim-turn2-giveitem-visible.png",
                     "/tmp/valheim-turn2-module-cron-visible.png"
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
