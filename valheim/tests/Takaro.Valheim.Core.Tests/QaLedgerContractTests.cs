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
        StringAssert.Contains(ledger, "Turn-4 source is not live-validated by this ledger");
    }

    [TestMethod]
    public void LedgerPinsTurnThreeArtifactAndExactPersistedEvidence()
    {
        var ledger = ReadLedger();

        foreach (var marker in new[]
                 {
                     "d0195b677c43a766daae55a226be4af73ef24a10",
                     "6273a722a98b1685bc87f22c5c4d1338c00ed28ea29e0b2c4ab1eae6d3d7a458",
                     "8d7818ae0642af9ec6f6e4e67acc236d79957c47d321d1e0dbf0e4da8777b567",
                     "/tmp/valheim-turn3-live-20260710T132532Z",
                     "e51c2951-ec59-4c1b-9be5-8eca3653a7f8",
                     "20897cd3-d833-4094-a68c-dfa4c6cf7f12",
                     "bdb561c6-3d43-4a88-8f64-4d6e224e916d",
                     "ba9b7643-923c-4a5a-bb0d-7739fe90a6e9",
                     "11,293",
                     "1,528,611",
                     "false-success fallback defect",
                     "f55c8b39-fc2c-442f-a4e2-be81a7851f4e",
                     "5ea168d7-1ae1-4d73-a7dc-5731b02957e5",
                     "command-executed analytics remained at zero",
                     "Turn-4 source is not live-validated"
                 })
        {
            StringAssert.Contains(ledger, marker);
        }
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
