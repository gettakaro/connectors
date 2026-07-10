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
        StringAssert.Contains(ledger, "bd4bfff718139d91cb415eece2c1b6b6c763942a");
        StringAssert.Contains(ledger, "b2b1748d266f7281731b992762ef7b3188a720de2224ccddb79745f3a271ac3d");
        StringAssert.Contains(ledger, "3477982857610212a83b006318bd0adea8f861afb7b090179016238a09a1e8b4");
        StringAssert.Contains(ledger, "Turn-6 source is pending");
    }

    [TestMethod]
    public void LedgerPinsTurnFiveLiveFailureAndStateIntegrityEvidenceWithoutPromotingTurnSix()
    {
        var ledger = ReadLedger();

        foreach (var marker in new[]
                 {
                     "145/145",
                     "13/13",
                     "real `net472` build: PASS with 0 warnings",
                     "Vanilla player `Hehe` connected",
                     "approximately 400 ms",
                     "approximately 434 ms",
                     "maximum 1000",
                     "expected integer",
                     "exactly one request and one response",
                     "approximately 891 ms",
                     "11 inventory requests",
                     "zero response frames",
                     "no fabricated inventory changes",
                     "player-connected and player-disconnected persisted",
                     "Exerciser: PASSED",
                     "Codex review: BLOCKED by quota until 20:22",
                     "/tmp/valheim-turn5-evidence",
                     "Turn-6 source is pending"
                 })
        {
            StringAssert.Contains(ledger, marker);
        }
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
                     "Turn-6 source is pending"
                 })
        {
            StringAssert.Contains(ledger, marker);
        }
    }

    [TestMethod]
    public void LedgerPinsTurnFourArtifactRuntimeAndKnownFailureAsHistoricalEvidence()
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
                     "Turn-6 source is pending"
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
