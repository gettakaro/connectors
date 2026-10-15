using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class Cs2PlayerStoreTests
{
    [TestMethod]
    public void LastKnownPlayerSurvivesRestartWithoutPersistingIp()
    {
        var root = Path.Combine(Path.GetTempPath(), "takaro-cs2-test-" + Guid.NewGuid().ToString("N"));
        var path = Path.Combine(root, "players.json");
        try
        {
            new Cs2PlayerStore(path).Remember(new TakaroPlayer("76561198000000001", "Limon", "76561198000000001", "steam:76561198000000001", "192.0.2.1", 12));
            var restored = new Cs2PlayerStore(path).Find("steam:76561198000000001");
            Assert.AreEqual("Limon", restored?.Name);
            Assert.AreEqual(false, restored?.Online);
            Assert.IsNull(restored?.Ip);
            Assert.AreEqual("76561198000000002", new Cs2PlayerStore(path).Find("76561198000000002")?.GameId);
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
    }
}
