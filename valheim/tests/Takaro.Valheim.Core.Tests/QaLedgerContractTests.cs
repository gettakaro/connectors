using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class QaLedgerContractTests
{
    [TestMethod]
    public void LedgerUsesRequiredVerdictAndPinsCurrentEvidenceBoundary()
    {
        var ledger = ReadLedger();

        StringAssert.Contains(ledger, "## Verdict: FAIL");
        StringAssert.Contains(ledger, "20bed2475ad558646c4c7cfccb20a185e516a429");
        StringAssert.Contains(ledger, "d322af0b405fbc901a48f5a5f0c1b9c1f052167ab05295acdc53896395a97186");
        StringAssert.Contains(ledger, "028eb5dfda9e52eb9998d3c538c4189e6332e761ad563a23ba8b76cdecc61755");
        StringAssert.Contains(ledger, "Turn-8 branch verification is pending");
        Assert.IsFalse(ledger.Contains("Turn-6 source is pending", StringComparison.Ordinal));
    }

    [TestMethod]
    public void LedgerPinsTurnSixLiveRuntimeAndStateIntegrityEvidence()
    {
        var ledger = ReadLedger();

        foreach (var marker in new[]
                 {
                     "158/158",
                     "19/19",
                     "real `net472` build: PASS with 0 warnings",
                     "Vanilla player `Hehe` connected",
                     "approximately 387 ms",
                     "zero response frames",
                     "no fabricated inventory changes",
                     "4dadfdf6-18a3-41f1-ae2c-b94200dea9ab",
                     "4e0aa0c0-d5da-4558-be9b-61c906b5bcfc",
                     "63c912ff-5c5e-402f-8f4e-1b31ece68ce3",
                     "85/36/-2",
                     "Exerciser: PASSED",
                     "Codex review: BLOCKED by quota until 20:22",
                     "/tmp/valheim-turn6-evidence",
                     "cleanup complete",
                     "Turn-8 branch verification is pending"
                 })
        {
            StringAssert.Contains(ledger, marker);
        }
    }

    [TestMethod]
    public void LedgerPinsTurnSevenFailedArtifactAndNumericControlEvidence()
    {
        var ledger = ReadLedger();

        foreach (var marker in new[]
                 {
                     "36730faec109f9975865492d9cc619ab12f5fc7f",
                     "7.8.9-rc.2+verify7",
                     "5d24cf113e1235c6b51844a5d3f4cbe2380be0e0105888aa92d191753bbfda88",
                     "bb74d96f6606736d66956b7cbe3746b5731c0921e38c07c4540f0022e0d6231a",
                     "because its version is invalid",
                     "numeric-version control",
                     "7.8.9",
                     "140/33/-2",
                     "247a346b-c69d-47b1-b9c9-d28cc4a74d60",
                     "aae4df31-7660-4447-8103-8447eb639518",
                     "Codex review: BLOCKED by quota until 20:22",
                     "Turn-8 branch verification is pending"
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
                     "command-executed analytics remained at zero"
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
                     "Invalid `giveItem`",
                     "timed out"
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

    [TestMethod]
    public void LedgerKeepsUnprovenDestructiveActionsApprovalGatedAndUnsupported()
    {
        var ledger = ReadLedger();

        StringAssert.Contains(ledger, "kickPlayer`, `banPlayer`, `unbanPlayer`, and `shutdown");
        StringAssert.Contains(ledger, "approval-gated");
        StringAssert.Contains(ledger, "exact live support remains unproven");
        StringAssert.Contains(ledger, "classified `unsupported`");
    }

    private static string ReadLedger()
    {
        var path = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../qa/2026-07-10-server-only-validation.md"));
        return File.ReadAllText(path);
    }
}
