using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class Cs2CapabilitiesTests
{
    private static string[] Names(IReadOnlyList<object> manifest) =>
        manifest.Select(entry => JsonSerializer.SerializeToElement(entry).GetProperty("name").GetString()!).ToArray();

    [TestMethod]
    public void DeclaresOnlyWhatThisServerCanDo()
    {
        var names = Names(Cs2Capabilities.Build(false, null, false, null));
        CollectionAssert.Contains(names, "players.list");
        CollectionAssert.Contains(names, "events.entity-killed");
        CollectionAssert.DoesNotContain(names, "server.shutdown");
        CollectionAssert.DoesNotContain(names, "map");
        CollectionAssert.DoesNotContain(names, "locations.list");
        Assert.AreEqual(names.Length, names.Distinct().Count());
    }

    [TestMethod]
    public void DeclaresProgressionAsClientReported()
    {
        var progression = Cs2Capabilities.Build(false, null, false, null).Select(entry => JsonSerializer.SerializeToElement(entry))
            .Single(entry => entry.GetProperty("name").GetString() == "players.progression");
        Assert.AreEqual("client-reported", progression.GetProperty("parameters").GetProperty("source").GetString());
    }

    [TestMethod]
    public void AddsOptionalCapabilitiesWhenAvailable()
    {
        var manifest = Cs2Capabilities.Build(true, new { id = "de_dust2" }, true, "1.41.9.6");
        var names = Names(manifest);
        CollectionAssert.Contains(names, "server.shutdown");
        CollectionAssert.Contains(names, "locations.list");
        var map = manifest.Select(entry => JsonSerializer.SerializeToElement(entry)).Single(entry => entry.GetProperty("name").GetString() == "map");
        Assert.AreEqual("de_dust2", map.GetProperty("parameters").GetProperty("dimensions")[0].GetProperty("id").GetString());
        var profile = manifest.Select(entry => JsonSerializer.SerializeToElement(entry)).Single(entry => entry.GetProperty("name").GetString() == "game.profile");
        Assert.AreEqual("1.41.9.6", profile.GetProperty("parameters").GetProperty("version").GetString());
    }
}
