using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class KnownPlayerNamesTests
{
    [TestMethod]
    public void NamesSurviveARestartAndMatchBareSteamBans()
    {
        var path = Path.Combine(Path.GetTempPath(), $"known-players-{Guid.NewGuid():N}.json");
        try
        {
            new KnownPlayerNames(path).Observe("Steam_76561198000735875", "Odin");

            var reloaded = new KnownPlayerNames(path);
            reloaded.Load();

            Assert.IsTrue(reloaded.TryGet("76561198000735875", out var bare));
            Assert.AreEqual("Odin", bare);
            Assert.IsTrue(reloaded.TryGet("steam_76561198000735875", out var prefixed));
            Assert.AreEqual("Odin", prefixed);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [TestMethod]
    public void IgnoresBlankNamesAndNamesThatAreJustTheId()
    {
        var names = new KnownPlayerNames();
        names.Observe("Xbox_2535405290924481", "");
        names.Observe("Xbox_2535405290924481", "Xbox_2535405290924481");

        Assert.IsFalse(names.TryGet("Xbox_2535405290924481", out _));

        names.Observe("Xbox_2535405290924481", "Thor");
        names.Observe("Xbox_2535405290924481", "Thor the Second");
        Assert.IsTrue(names.TryGet("Xbox_2535405290924481", out var latest));
        Assert.AreEqual("Thor the Second", latest);
    }
}
