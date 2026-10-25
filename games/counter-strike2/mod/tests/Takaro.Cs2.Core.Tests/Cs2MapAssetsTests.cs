using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using StbImageSharp;
using StbImageWriteSharp;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class Cs2MapAssetsTests
{
    [TestMethod]
    public void ServesCalibratedTileAndSpawnLocations()
    {
        var directory = Path.Combine(Path.GetTempPath(), "cs2-map-test-" + Guid.NewGuid());
        Directory.CreateDirectory(directory);
        try
        {
            var source = new byte[128 * 128 * 4];
            for (var i = 0; i < source.Length; i += 4)
            {
                source[i] = (byte)(i / 4 % 128);
                source[i + 1] = 70;
                source[i + 2] = 30;
                source[i + 3] = 255;
            }
            using (var file = File.Create(Path.Combine(directory, "de_test.png")))
                new ImageWriter().WritePng(source, 128, 128, StbImageWriteSharp.ColorComponents.RedGreenBlueAlpha, file);
            File.WriteAllText(Path.Combine(directory, "de_test.txt"),
                "\"de_test\"\n{\n\"pos_x\" \"-256\"\n\"pos_y\" \"256\"\n\"scale\" \"4\"\n}");
            File.WriteAllText(Path.Combine(directory, "de_test.vents"),
                "====1====\nclassname \"info_player_terrorist\"\norigin [ -100, 80, 20 ]\n" +
                "====2====\nclassname \"info_player_terrorist\"\norigin [ -80, 60, 20 ]\n" +
                "====3====\nclassname \"info_player_counterterrorist\"\norigin [ 100, 90, -10 ]\n");

            var assets = new Cs2MapAssets(directory);
            var info = JsonSerializer.SerializeToElement(assets.GetInfo("de_test").Payload);
            Assert.IsTrue(info.GetProperty("enabled").GetBoolean());
            Assert.AreEqual(128, info.GetProperty("mapBlockSize").GetInt32());
            Assert.AreEqual(512, info.GetProperty("mapSizeX").GetInt32());
            var locations = JsonSerializer.SerializeToElement(assets.ListLocations("de_test").Payload);
            Assert.AreEqual(2, locations.GetArrayLength());
            Assert.AreEqual("t_spawn", locations[0].GetProperty("code").GetString());
            Assert.AreEqual(-100d, locations[0].GetProperty("position").GetProperty("x").GetDouble());
            Assert.AreEqual(20d, locations[0].GetProperty("position").GetProperty("z").GetDouble());

            var tile = assets.GetTile("de_test", -1, 0, 4);
            Assert.IsTrue(tile.Success);
            var png = Convert.FromBase64String((string)tile.Payload!);
            var image = ImageResult.FromMemory(png, StbImageSharp.ColorComponents.RedGreenBlueAlpha);
            Assert.AreEqual(128, image.Width);
            Assert.AreEqual(128, image.Height);
            Assert.AreEqual(70, image.Data[1]);
            Assert.AreEqual(255, image.Data[3]);
            Assert.AreEqual("tile_out_of_range", assets.GetTile("de_test", 50, 50, 4).Error?.Code);
            Assert.AreEqual("invalid_args", assets.GetTile("de_test", 0, 0, 5).Error?.Code);
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }

    [TestMethod]
    public void MissingOrInvalidMapNeverClaimsSuccess()
    {
        var assets = new Cs2MapAssets(Path.GetTempPath());
        Assert.IsFalse(JsonSerializer.SerializeToElement(assets.GetInfo("not_installed").Payload)
            .GetProperty("enabled").GetBoolean());
        Assert.AreEqual("map_unavailable", assets.ListLocations("not_installed").Error?.Code);
        Assert.AreEqual("map_unavailable", assets.GetTile("../secret", 0, 0, 0).Error?.Code);
    }
}
