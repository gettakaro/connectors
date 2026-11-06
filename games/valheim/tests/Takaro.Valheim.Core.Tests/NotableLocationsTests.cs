using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class NotableLocationsTests
{
    [TestMethod]
    public void KeepsOnlyNotablePlacesUnderTheirEnglishNames()
    {
        var locations = NotableLocations.Select(
        [
            new ValheimLocationInstance("Runestone_Meadows", 10, 30, 10),
            new ValheimLocationInstance("Greydwarf_camp1", 20, 30, 20),
            new ValheimLocationInstance("StartTemple", 2.04, 31.26, -3.96),
            new ValheimLocationInstance("Eikthyrnir", -412.6, 40, 288.4),
            new ValheimLocationInstance("Vendor_BlackForest", 3000, 50, -2000),
        ]);

        CollectionAssert.AreEqual(
            new[] { "Eikthyr Altar", "Haldor the Trader", "Sacrificial Stones" },
            locations.Select(location => location.Name).ToArray());
        var temple = locations.Single(location => location.Name == "Sacrificial Stones");
        Assert.AreEqual("StartTemple@2_-4", temple.Code);
        Assert.AreEqual(new TakaroPosition(2, 31.3, -4, "valheim"), temple.Position);
    }

    [TestMethod]
    public void EveryNamedPrefabHasAHumanName()
    {
        foreach (var prefab in NotableLocations.Prefabs)
        {
            Assert.IsTrue(NotableLocations.TryGetDisplayName(prefab, out var name), prefab);
            Assert.AreNotEqual(prefab, name, "a display name, not the prefab code");
            Assert.IsFalse(name.Contains('_'), name);
            Assert.IsFalse(name.StartsWith("$", StringComparison.Ordinal), name);
        }

        CollectionAssert.IsSubsetOf(
            new[] { "StartTemple", "Eikthyrnir", "GDKing", "Bonemass", "Dragonqueen", "GoblinKing",
                "Mistlands_DvergrBossEntrance1", "FaderLocation", "Vendor_BlackForest", "Hildir_camp", "BogWitch_Camp" },
            NotableLocations.Prefabs.ToArray());
    }

    [TestMethod]
    public void EachPlacedInstanceIsItsOwnLocation()
    {
        var locations = NotableLocations.Select(
        [
            new ValheimLocationInstance("Eikthyrnir", 100, 40, 100),
            new ValheimLocationInstance("Eikthyrnir", -900, 40, 700),
            new ValheimLocationInstance("Eikthyrnir", 100.2, 40, 99.8),
        ]);

        CollectionAssert.AreEqual(
            new[] { "Eikthyrnir@-900_700", "Eikthyrnir@100_100" },
            locations.Select(location => location.Code).ToArray());
        Assert.IsTrue(locations.All(location => location.Name == "Eikthyr Altar"));
    }

    [TestMethod]
    public void SerializesAsTheLocationDtoInTheMapDimension()
    {
        var location = NotableLocations.Select([new ValheimLocationInstance("GDKing", 100, 20.5, -75)]).Single();

        using var document = JsonDocument.Parse(TakaroProtocol.CreateResponse("locations", TakaroActionResult.Ok(new[] { location })));
        var payload = document.RootElement.GetProperty("payload")[0];

        Assert.AreEqual("GDKing@100_-75", payload.GetProperty("code").GetString());
        Assert.AreEqual("The Elder Altar", payload.GetProperty("name").GetString());
        var position = payload.GetProperty("position");
        Assert.AreEqual(100, position.GetProperty("x").GetDouble());
        Assert.AreEqual(20.5, position.GetProperty("y").GetDouble());
        Assert.AreEqual(-75, position.GetProperty("z").GetDouble());
        Assert.AreEqual(WorldMapFrame.DimensionId, position.GetProperty("dimension").GetString());
        Assert.IsFalse(payload.TryGetProperty("x", out _), "ILocationDTO coordinates belong under position.");
        Assert.IsFalse(payload.TryGetProperty("dimension", out _), "ILocationDTO dimension belongs under position.");
    }

    [TestMethod]
    public void AWorldWithoutNotablePlacesIsEmpty()
    {
        Assert.AreEqual(0, NotableLocations.Select([new ValheimLocationInstance("Runestone_Meadows", 1, 2, 3)]).Length);
    }
}
