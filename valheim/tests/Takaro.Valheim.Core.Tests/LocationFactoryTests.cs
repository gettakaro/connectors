using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class LocationFactoryTests
{
    [TestMethod]
    public void CreatesTakaroLocationFromValheimLocationInstance()
    {
        var location = LocationFactory.Create(
            code: "Runestone_BlackForest",
            rawName: "$location_runestone",
            x: 100,
            y: 20.5,
            z: -75);

        using var document = JsonDocument.Parse(TakaroProtocol.CreateResponse("locations", TakaroActionResult.Ok(new[] { location })));
        var payload = document.RootElement.GetProperty("payload")[0];

        Assert.AreEqual("Runestone_BlackForest", payload.GetProperty("code").GetString());
        Assert.AreEqual("location_runestone", payload.GetProperty("name").GetString());
        Assert.AreEqual(100, payload.GetProperty("x").GetDouble());
        Assert.AreEqual(20.5, payload.GetProperty("y").GetDouble());
        Assert.AreEqual(-75, payload.GetProperty("z").GetDouble());
        Assert.AreEqual("valheim", payload.GetProperty("dimension").GetString());
    }

    [TestMethod]
    public void FallsBackToCodeWhenLocationNameIsEmpty()
    {
        var location = LocationFactory.Create("StartTemple", "", 0, 0, 0);

        Assert.AreEqual("StartTemple", location.Name);
    }
}
