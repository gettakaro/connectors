using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class Cs2BanStoreTests
{
    [TestMethod]
    public void BanPersistsAcrossRestartAndExpires()
    {
        var root = Path.Combine(Path.GetTempPath(), "takaro-cs2-test-" + Guid.NewGuid().ToString("N"));
        var path = Path.Combine(root, "bans.json");
        try
        {
            var first = new Cs2BanStore(path);
            first.Add("76561198000000001", "Player", "test", DateTimeOffset.UtcNow.AddMinutes(1));
            Assert.IsTrue(new Cs2BanStore(path).IsBanned("76561198000000001"));
            first.Add("76561198000000002", "Expired", "test", DateTimeOffset.UtcNow.AddSeconds(-1));
            Assert.IsFalse(new Cs2BanStore(path).IsBanned("76561198000000002"));
            Assert.IsTrue(new Cs2BanStore(path).Remove("76561198000000001"));
            Assert.IsFalse(new Cs2BanStore(path).IsBanned("76561198000000001"));
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
    }
}
