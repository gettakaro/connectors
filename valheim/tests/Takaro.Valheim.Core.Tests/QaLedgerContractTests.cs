using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class QaLedgerContractTests
{
    [TestMethod]
    public void LedgerUsesRequiredVerdictAndPinsCurrentEvidenceBoundary()
    {
        var ledger = ReadLedger();

        StringAssert.Contains(ledger, "## Verdict: PASS WITH GAPS");
        StringAssert.Contains(ledger, "75224f2cc9540f9e40baa6178e4ffb70d247b892");
        StringAssert.Contains(ledger, "35238e55dd4353374cba26565c2e5daa66de70d5c4d22a5823941d515ea34b6b");
        StringAssert.Contains(ledger, "58e6615b1c078d0f85e86beac9d65eed3d949d3b5e9bf117334421e72db8fb02");
        StringAssert.Contains(ledger, "Turn-5 source is not live-validated by this ledger");
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
                     "Turn-5 source is not live-validated"
                 })
        {
            StringAssert.Contains(ledger, marker);
        }
    }

    [TestMethod]
    public void LedgerPinsTurnFourArtifactRuntimeAndKnownFailureWithoutClaimingTurnFiveLive()
    {
        var ledger = ReadLedger();

        foreach (var marker in new[]
                 {
                     "35238e55",
                     "34b6b",
                     "58e6615b",
                     "fb02",
                     "121/121",
                     "8/8",
                     "Codex review: COMPLETED",
                     "Vanilla player `Hehe`",
                     "80/36/-2",
                     "85/36/-2",
                     "e93ed6d1-29f1-49f7-9bf7-43d4d625f395",
                     "aee52332-392f-449b-ba92-521ef66b3b71",
                     "11,293",
                     "1,815,046",
                     "dd7fabcb-bd18-491c-8ea3-c9d2147be33f",
                     "/tmp/valheim-turn4-visible-actions.png",
                     "/tmp/valheim-turn4-evidence/raw-harness-result.json",
                     "invalid `giveItem`",
                     "timed out",
                     "Turn-5 source is not live-validated"
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
