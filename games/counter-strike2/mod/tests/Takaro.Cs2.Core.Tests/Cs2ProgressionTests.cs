using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class Cs2ProgressionTests
{
    private const ulong SteamId = 76561198000000001UL;

    private static Cs2Player Player(int rank, bool bot = false) => new("Alice", SteamId, null, null, bot, rank);

    [TestMethod]
    public void ReportsTheProfileRankAsTheCharacterTrack()
    {
        var progression = new Cs2Progression();
        progression.Observe(Player(7));
        var tracks = JsonSerializer.SerializeToElement(progression.Read(SteamId.ToString(), true));
        Assert.AreEqual(1, tracks.GetArrayLength());
        Assert.AreEqual("character", tracks[0].GetProperty("key").GetString());
        Assert.AreEqual("Profile Rank", tracks[0].GetProperty("name").GetString());
        Assert.AreEqual(7, tracks[0].GetProperty("level").GetInt32());
        Assert.IsFalse(tracks[0].TryGetProperty("experience", out _));
    }

    [TestMethod]
    public void KeepsTheLastRankAfterThePlayerLeavesAndFollowsALowerRank()
    {
        var progression = new Cs2Progression();
        progression.Observe(Player(7));
        progression.Observe(Player(0));
        Assert.AreEqual(7, JsonSerializer.SerializeToElement(progression.Read(SteamId.ToString(), false))[0].GetProperty("level").GetInt32());
        progression.Observe(Player(1));
        Assert.AreEqual(1, JsonSerializer.SerializeToElement(progression.Read(SteamId.ToString(), false))[0].GetProperty("level").GetInt32());
    }

    [TestMethod]
    public void NeverInventsARank()
    {
        var progression = new Cs2Progression();
        progression.Observe(Player(0));
        progression.Observe(Player(5, bot: true));
        Assert.AreEqual(0, JsonSerializer.SerializeToElement(progression.Read(SteamId.ToString(), true)).GetArrayLength());
        Assert.IsNull(progression.Read(SteamId.ToString(), false));
    }
}
