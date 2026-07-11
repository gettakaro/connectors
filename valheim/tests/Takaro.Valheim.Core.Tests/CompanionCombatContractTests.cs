using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Companion;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionCombatContractTests
{
    [TestMethod]
    public void LocalDeathGetsOneEventIdAndDuplicateCallbackIsSuppressed()
    {
        var reader = new CompanionCombatReader();

        Assert.IsTrue(reader.TryReserveEvent(
            "player-death",
            "player-17",
            TimeSpan.Zero,
            out var first));
        Assert.IsFalse(reader.TryReserveEvent(
            "player-death",
            "player-17",
            TimeSpan.FromSeconds(1),
            out _));
        Assert.IsTrue(reader.TryReserveEvent(
            "player-death",
            "player-17",
            CompanionCombatReader.DuplicateWindow,
            out var later));

        Assert.IsNotNull(first);
        Assert.IsNotNull(later);
        Assert.AreNotEqual(first, later);
    }

    [TestMethod]
    public void DistinctEntityKillsGetDistinctBoundedEventIds()
    {
        var reader = new CompanionCombatReader();

        Assert.IsTrue(reader.TryReserveEvent(
            "entity-killed",
            "boar-1",
            TimeSpan.Zero,
            out var first));
        Assert.IsTrue(reader.TryReserveEvent(
            "entity-killed",
            "boar-2",
            TimeSpan.Zero,
            out var second));

        Assert.IsNotNull(first);
        Assert.IsNotNull(second);
        Assert.AreNotEqual(first, second);
        Assert.IsTrue(first.Length <= 64);
        Assert.IsTrue(second.Length <= 64);
    }

    [TestMethod]
    public void ResetAllowsCurrentSourceToEmitAgain()
    {
        var reader = new CompanionCombatReader();
        Assert.IsTrue(reader.TryReserveEvent(
            "player-death",
            "player-17",
            TimeSpan.Zero,
            out _));
        Assert.IsFalse(reader.TryReserveEvent(
            "player-death",
            "player-17",
            TimeSpan.Zero,
            out _));

        reader.Reset();

        Assert.IsTrue(reader.TryReserveEvent(
            "player-death",
            "player-17",
            TimeSpan.Zero,
            out _));
    }

    [TestMethod]
    public void HooksRequireLocalDeathAndLocalAttackerOnNonPlayerEntity()
    {
        var source = ReadCompanionSource("CompanionClientHooks.cs");

        StringAssert.Contains(source, "__instance != Player.m_localPlayer");
        StringAssert.Contains(source, "character is Player");
        StringAssert.Contains(source, "character.GetComponent<Player>() != null");
        StringAssert.Contains(source, "hit?.GetAttacker() != Player.m_localPlayer");
        StringAssert.Contains(source, "combatReader.TryCreateLocalPlayerDeath(");
        StringAssert.Contains(source, "combatReader.TryCreateEntityKilled(");
        StringAssert.Contains(source, "activeBridge.TrySendPlayerDeath(report)");
        StringAssert.Contains(source, "activeBridge.TrySendEntityKilled(report)");
    }

    [TestMethod]
    public void CombatReaderUsesCachedLastHitAndRealWeaponHints()
    {
        var source = ReadCompanionSource("CompanionCombatReader.cs");

        StringAssert.Contains(source, "AccessTools.Field(typeof(Character), \"m_lastHit\")");
        StringAssert.Contains(source, "LastHitField?.GetValue(character) as HitData");
        StringAssert.Contains(source, "hit.GetAttacker() as Humanoid");
        StringAssert.Contains(source, "attacker?.GetCurrentWeapon()");
        StringAssert.Contains(source, "weapon.m_dropPrefab");
        StringAssert.Contains(source, "weapon.m_shared?.m_name");
        StringAssert.Contains(source, "hit.m_skill.ToString()");
        StringAssert.Contains(source, "?? \"unarmed\"");
        StringAssert.Contains(source, "return null;");
    }

    [TestMethod]
    public void HarmonyPatchesOnlyPlayerAndCharacterOnDeathPostfixes()
    {
        var source = ReadCompanionSource("CompanionClientHooks.cs");

        StringAssert.Contains(source, "[HarmonyPatch(typeof(Player), \"OnDeath\")]");
        StringAssert.Contains(source, "[HarmonyPatch(typeof(Character), \"OnDeath\")]");
        StringAssert.Contains(source, "CompanionClientHooks.OnLocalPlayerDeath(__instance)");
        StringAssert.Contains(source, "CompanionClientHooks.OnCharacterDeath(__instance)");
        Assert.IsFalse(source.Contains("RPC_OnDeath", StringComparison.Ordinal));
    }

    private static string ReadCompanionSource(string fileName) =>
        File.ReadAllText(Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../src/Takaro.Valheim.Companion",
            fileName)));
}
