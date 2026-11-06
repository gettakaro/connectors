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
            var dimension = JsonSerializer.SerializeToElement(assets.DescribeDimension("de_test"));
            Assert.AreEqual("de_test", dimension.GetProperty("id").GetString());
            Assert.AreEqual(-256d, dimension.GetProperty("bounds").GetProperty("horizontal").GetProperty("min").GetDouble());
            Assert.AreEqual(256d, dimension.GetProperty("bounds").GetProperty("vertical").GetProperty("max").GetDouble());
            Assert.AreEqual("up", dimension.GetProperty("axes").GetProperty("vertical").GetProperty("direction").GetString());
            Assert.AreEqual(256, dimension.GetProperty("source").GetProperty("tileSize").GetInt32());
            Assert.IsTrue(assets.HasLocations("de_test"));
            var locations = JsonSerializer.SerializeToElement(assets.ListLocations("de_test").Payload);
            Assert.AreEqual(2, locations.GetArrayLength());
            Assert.AreEqual("t_spawn", locations[0].GetProperty("code").GetString());
            Assert.AreEqual(-100d, locations[0].GetProperty("position").GetProperty("x").GetDouble());
            Assert.AreEqual(20d, locations[0].GetProperty("position").GetProperty("z").GetDouble());
            Assert.AreEqual("de_test", locations[0].GetProperty("position").GetProperty("dimension").GetString());
            Assert.AreEqual("spawn", locations[0].GetProperty("category").GetString());

            var tile = assets.GetTile("de_test", 0, 0, 3);
            Assert.IsTrue(tile.Success);
            var payload = JsonSerializer.SerializeToElement(tile.Payload);
            Assert.AreEqual("image/png", payload.GetProperty("mimeType").GetString());
            var png = Convert.FromBase64String(payload.GetProperty("data").GetString()!);
            var image = ImageResult.FromMemory(png, StbImageSharp.ColorComponents.RedGreenBlueAlpha);
            Assert.AreEqual(256, image.Width);
            Assert.AreEqual(256, image.Height);
            Assert.AreEqual(70, image.Data[1]);
            Assert.AreEqual(255, image.Data[3]);
            Assert.AreEqual("tile_out_of_range", assets.GetTile("de_test", 8, 0, 3).Error?.Code);
            Assert.AreEqual("invalid_args", assets.GetTile("de_test", 0, 0, 4).Error?.Code);
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
        Assert.IsNull(assets.DescribeDimension("not_installed"));
        Assert.AreEqual("map_unavailable", assets.ListLocations("not_installed").Error?.Code);
        Assert.AreEqual("map_unavailable", assets.GetTile("../secret", 0, 0, 0).Error?.Code);
    }
}
