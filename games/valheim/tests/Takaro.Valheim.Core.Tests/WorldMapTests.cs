using System.IO.Compression;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class WorldMapTests
{
    [TestMethod]
    public void TileSizeHalvesPerZoomAndIsOneMetrePerPixelAtMaxZoom()
    {
        Assert.AreEqual(2048d, WorldMapGeometry.TileWorldSize(0));
        Assert.AreEqual(128d, WorldMapGeometry.TileWorldSize(WorldMapGeometry.MaxZoom));
        Assert.AreEqual(1d, WorldMapGeometry.TileWorldSize(WorldMapGeometry.MaxZoom) / WorldMapGeometry.TilePixels);
    }

    [TestMethod]
    public void PixelRowZeroIsTheNorthEdgeAndTileYGrowsNorth()
    {
        // Tile 0/0 at max zoom covers X 0..128, Z 0..128. Row 0 is its northern edge.
        var (nwX, nwZ) = WorldMapGeometry.PixelCenter(4, 0, 0, 0, 0);
        var (seX, seZ) = WorldMapGeometry.PixelCenter(4, 0, 0, 127, 127);
        Assert.AreEqual(0.5, nwX, 1e-9);
        Assert.AreEqual(127.5, nwZ, 1e-9);
        Assert.AreEqual(127.5, seX, 1e-9);
        Assert.AreEqual(0.5, seZ, 1e-9);

        // Tile y = -1 is south of the origin.
        var (_, southZ) = WorldMapGeometry.PixelCenter(4, 0, -1, 0, 0);
        Assert.IsTrue(southZ < 0);
    }

    [DataTestMethod]
    [DataRow(0, 0d, 0d)]
    [DataRow(4, 123.4, -987.6)]
    [DataRow(2, -10123.9, 9876.5)]
    [DataRow(0, 4321d, -4321d)]
    public void LocateIsTheInverseOfPixelCenter(int zoom, double worldX, double worldZ)
    {
        var (tileX, tileY, column, row) = WorldMapGeometry.Locate(zoom, worldX, worldZ);
        var (centreX, centreZ) = WorldMapGeometry.PixelCenter(zoom, tileX, tileY, column, row);
        var halfPixel = WorldMapGeometry.TileWorldSize(zoom) / WorldMapGeometry.TilePixels / 2;

        Assert.AreEqual(worldX, centreX, halfPixel + 1e-9);
        Assert.AreEqual(worldZ, centreZ, halfPixel + 1e-9);
    }

    [TestMethod]
    public void MatchesThePlayerMapLeafletTileConvention()
    {
        // The Takaro player map uses scale(zoom) = 2^(zoom+3), latLng = (z/128, x/128) and
        // requests Takaro tile y = -leafletY - 1. Reproduce its maths for a player position
        // and check the connector puts that point in the same tile and pixel.
        const double playerX = -812.25;
        const double playerZ = 1543.75;
        for (var zoom = 0; zoom <= WorldMapGeometry.MaxZoom; zoom++)
        {
            var scale = Math.Pow(2, zoom + 3);
            var pixelX = playerX / 128 * scale;
            var pixelY = -(playerZ / 128) * scale;
            var leafletX = (int)Math.Floor(pixelX / 128);
            var leafletY = (int)Math.Floor(pixelY / 128);
            var expectedColumn = (int)Math.Floor(pixelX - leafletX * 128);
            var expectedRow = (int)Math.Floor(pixelY - leafletY * 128);

            var (tileX, tileY, column, row) = WorldMapGeometry.Locate(zoom, playerX, playerZ);

            Assert.AreEqual(leafletX, tileX, $"zoom {zoom} tile x");
            Assert.AreEqual(-leafletY - 1, tileY, $"zoom {zoom} tile y");
            Assert.AreEqual(expectedColumn, column, $"zoom {zoom} column");
            Assert.AreEqual(expectedRow, row, $"zoom {zoom} row");
        }
    }

    [TestMethod]
    public void OnlyTilesOverlappingTheWorldAreInRange()
    {
        Assert.IsTrue(WorldMapGeometry.IntersectsWorld(0, 0, 0));
        Assert.IsTrue(WorldMapGeometry.IntersectsWorld(0, -6, -6));
        Assert.IsTrue(WorldMapGeometry.IntersectsWorld(0, 5, 5));
        Assert.IsFalse(WorldMapGeometry.IntersectsWorld(0, 6, 0));
        Assert.IsFalse(WorldMapGeometry.IntersectsWorld(0, -7, 0));
        Assert.IsTrue(WorldMapGeometry.IntersectsWorld(4, 82, -83));
        Assert.IsFalse(WorldMapGeometry.IntersectsWorld(4, 83, 0));
        Assert.IsFalse(WorldMapGeometry.IntersectsWorld(5, 0, 0));
        Assert.IsFalse(WorldMapGeometry.IntersectsWorld(-1, 0, 0));
    }

    [TestMethod]
    public void MapInfoSatisfiesTakaroMapInfoDtoFields()
    {
        var json = JsonSerializer.Serialize(WorldMapGeometry.CreateMapInfo(true));
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;

        Assert.AreEqual(JsonValueKind.True, root.GetProperty("enabled").ValueKind);
        Assert.AreEqual(128, root.GetProperty("mapBlockSize").GetInt32());
        Assert.AreEqual(4, root.GetProperty("maxZoom").GetInt32());
        Assert.AreEqual(21000, root.GetProperty("mapSizeX").GetInt32());
        Assert.AreEqual(21000, root.GetProperty("mapSizeY").GetInt32());
        Assert.AreEqual(21000, root.GetProperty("mapSizeZ").GetInt32());
        Assert.AreEqual(6, root.EnumerateObject().Count());
    }

    [TestMethod]
    public void RasterRoundTripsSamplesAndRejectsAnotherWorld()
    {
        var raster = BuildRaster(16, 42, (x, z) => new WorldMapSample(
            x < 0 ? WorldMapBiome.Meadows : WorldMapBiome.Mountain,
            (float)(z / 100.0),
            z > 0));

        var bytes = raster.Serialize();
        var loaded = WorldMapRaster.TryDeserialize(bytes, 16, 42);

        Assert.IsNotNull(loaded);
        for (var row = 0; row < 16; row++)
        {
            for (var column = 0; column < 16; column++)
            {
                Assert.AreEqual(raster.BiomeAt(row, column), loaded!.BiomeAt(row, column));
                Assert.AreEqual(raster.ForestAt(row, column), loaded.ForestAt(row, column));
                Assert.AreEqual(raster.HeightAt(row, column), loaded.HeightAt(row, column), 1e-4);
            }
        }

        Assert.IsNull(WorldMapRaster.TryDeserialize(bytes, 16, 43));
        Assert.IsNull(WorldMapRaster.TryDeserialize(bytes, 32, 42));
        Assert.IsNull(WorldMapRaster.TryDeserialize(bytes[..^1], 16, 42));
        Assert.IsNull(WorldMapRaster.TryDeserialize([1, 2, 3], 16, 42));
    }

    [TestMethod]
    public void RasterRowsRunSouthToNorthAndColumnsWestToEast()
    {
        var raster = BuildRaster(4, 1, (x, z) => new WorldMapSample(WorldMapBiome.Meadows, (float)x, false));

        Assert.IsTrue(raster.CellCenter(0) < 0);
        Assert.IsTrue(raster.CellCenter(3) > 0);
        Assert.IsTrue(raster.HeightAt(0, 0) < raster.HeightAt(0, 3));
        Assert.AreEqual((0, 3), raster.CellOf(9000, -9000));
        Assert.AreEqual((3, 0), raster.CellOf(-9000, 9000));
    }

    [TestMethod]
    public void BuilderStopsWhenTheFrameBudgetIsSpentAndResumes()
    {
        var calls = 0;
        var builder = new WorldMapRasterBuilder(32, 7, (_, _) =>
        {
            calls++;
            return new WorldMapSample(WorldMapBiome.Plains, 50, false);
        });

        Assert.IsFalse(builder.Step(() => false));
        Assert.AreEqual(64, calls);
        Assert.IsFalse(builder.IsComplete);

        Assert.IsTrue(builder.Step(() => true));
        Assert.AreEqual(32 * 32, calls);
        Assert.AreEqual(1d, builder.Progress);
    }

    [TestMethod]
    public void RendersWaterLandAndOutsideTheWorldDifferently()
    {
        // Land east of x=0, sea west of it.
        var raster = BuildRaster(256, 1, (x, z) => x >= 0
            ? new WorldMapSample(WorldMapBiome.Meadows, 40, false)
            : new WorldMapSample(WorldMapBiome.Ocean, -20, false));

        var land = WorldMapTileRenderer.ColorAt(raster, 2000, 0, 82);
        var sea = WorldMapTileRenderer.ColorAt(raster, -2000, 0, 82);
        var outside = WorldMapTileRenderer.ColorAt(raster, 10600, 0, 82);

        Assert.AreEqual(WorldMapTileRenderer.BiomeColor(WorldMapBiome.Meadows), land);
        Assert.IsTrue(sea.B > sea.R && sea.B > sea.G, "sea is blue");
        Assert.AreNotEqual(land, outside);
        Assert.AreNotEqual(sea, outside);
    }

    [TestMethod]
    public void ForestDarkensTheBiomeColour()
    {
        var raster = BuildRaster(64, 1, (x, _) => new WorldMapSample(WorldMapBiome.Meadows, 40, x >= 0));

        var forest = WorldMapTileRenderer.ColorAt(raster, 3000, 0, 400);
        var open = WorldMapTileRenderer.ColorAt(raster, -3000, 0, 400);

        Assert.IsTrue(forest.G < open.G);
    }

    [TestMethod]
    public void TileImageIsAValidPngThatDecodesToTheRenderedPixels()
    {
        var raster = BuildRaster(128, 1, (x, z) => new WorldMapSample(
            x >= 0 ? WorldMapBiome.Plains : WorldMapBiome.BlackForest,
            (float)(40 + z / 1000.0),
            false));
        var rgb = WorldMapTileRenderer.RenderRgb(raster, 0, -1, 0);

        foreach (var compress in new[] { true, false })
        {
            var png = PngEncoder.EncodeRgb(128, 128, rgb, compress);
            CollectionAssert.AreEqual(rgb, DecodeRgb(png, 128, 128), $"compress={compress}");
        }
    }

    [TestMethod]
    public void ServiceReportsDisabledUntilARasterIsPublished()
    {
        var service = new WorldMapService();
        service.SetStatus("the world map is being rendered (40%)");

        var info = JsonSerializer.Serialize(service.GetMapInfo().Payload);
        var tile = service.GetMapTile(0, 0, 0);

        StringAssert.Contains(info, "\"enabled\":false");
        Assert.IsFalse(tile.Success);
        Assert.AreEqual("map_not_ready", tile.ErrorCode);
        StringAssert.Contains(tile.Message, "40%");
    }

    [TestMethod]
    public void ServiceServesBase64PngTilesAndRejectsOutOfRangeOnes()
    {
        var service = new WorldMapService();
        service.Publish(BuildRaster(64, 1, (_, _) => new WorldMapSample(WorldMapBiome.Meadows, 40, false)));

        var info = JsonSerializer.Serialize(service.GetMapInfo().Payload);
        var tile = service.GetMapTile(-1, 0, 2);
        var cached = service.GetMapTile(-1, 0, 2);
        var outside = service.GetMapTile(40, 0, 2);
        var badZoom = service.GetMapTile(0, 0, 5);

        StringAssert.Contains(info, "\"enabled\":true");
        Assert.IsTrue(tile.Success);
        var png = Convert.FromBase64String((string)tile.Payload!);
        CollectionAssert.AreEqual(new byte[] { 0x89, 0x50, 0x4E, 0x47 }, png[..4]);
        Assert.AreSame(tile.Payload, cached.Payload);
        Assert.AreEqual("tile_out_of_range", outside.ErrorCode);
        Assert.AreEqual("invalid_args", badZoom.ErrorCode);
    }

    [TestMethod]
    public void PublishingANewWorldDropsCachedTiles()
    {
        var service = new WorldMapService();
        service.Publish(BuildRaster(32, 1, (_, _) => new WorldMapSample(WorldMapBiome.Meadows, 40, false)));
        var first = service.GetMapTile(0, 0, 0);

        service.Publish(BuildRaster(32, 2, (_, _) => new WorldMapSample(WorldMapBiome.AshLands, 40, false)));
        var second = service.GetMapTile(0, 0, 0);

        Assert.AreNotEqual(first.Payload, second.Payload);
    }

    [TestMethod]
    public void TileCacheEvictsLeastRecentlyUsedTiles()
    {
        var service = new WorldMapService(cacheCapacity: 2);
        service.Publish(BuildRaster(32, 1, (_, _) => new WorldMapSample(WorldMapBiome.Meadows, 40, false)));

        var a = service.GetMapTile(0, 0, 0);
        service.GetMapTile(1, 0, 0);
        service.GetMapTile(0, 0, 0);
        service.GetMapTile(2, 0, 0);
        var aAgain = service.GetMapTile(0, 0, 0);

        Assert.AreSame(a.Payload, aAgain.Payload);
    }

    [TestMethod]
    public void ConcurrentTileRequestsAreSafe()
    {
        var service = new WorldMapService(cacheCapacity: 8);
        service.Publish(BuildRaster(64, 1, (x, _) => new WorldMapSample(WorldMapBiome.Swamp, (float)(35 + x / 500), false)));

        var results = Enumerable.Range(0, 64)
            .AsParallel()
            .Select(i => service.GetMapTile(i % 10 - 5, i % 7 - 3, 0))
            .ToArray();

        Assert.IsTrue(results.All(result => result.Success));
    }

    [TestMethod]
    public void VersionOneMapCapabilityFollowsTheProtocolRules()
    {
        var capability = WorldMapFrame.CreateCapability();
        var dimension = capability["parameters"]!["dimensions"]!.AsArray().Single()!;
        var source = dimension["source"]!;
        var bounds = dimension["bounds"]!;

        Assert.AreEqual("map", capability["name"]!.GetValue<string>());
        StringAssert.Matches(dimension["id"]!.GetValue<string>(), new System.Text.RegularExpressions.Regex("^[A-Za-z0-9][A-Za-z0-9_.:-]{0,63}$"));
        Assert.AreEqual("valheim", dimension["id"]!.GetValue<string>(), "the dimension every Valheim position reports");
        Assert.AreEqual("tiled", source["type"]!.GetValue<string>());
        Assert.IsTrue(source["tileSize"]!.GetValue<int>() is >= 64 and <= 1024);
        Assert.IsTrue(source["minZoom"]!.GetValue<int>() <= source["maxZoom"]!.GetValue<int>());
        Assert.AreNotEqual(dimension["axes"]!["horizontal"]!["field"]!.GetValue<string>(), dimension["axes"]!["vertical"]!["field"]!.GetValue<string>());
        Assert.IsTrue(bounds["horizontal"]!["min"]!.GetValue<double>() < bounds["horizontal"]!["max"]!.GetValue<double>());
        Assert.IsTrue(bounds["vertical"]!["min"]!.GetValue<double>() < bounds["vertical"]!["max"]!.GetValue<double>());
    }

    [TestMethod]
    public void VersionOneTilesFollowTheXyzSchemeWithNorthUp()
    {
        var (topLeftX, topLeftZ) = WorldMapFrame.PixelCenter(0, 0, 0, 0, 0);
        var (zoom1X, zoom1Z) = WorldMapFrame.PixelCenter(1, 1, 1, 0, 0);

        Assert.IsTrue(topLeftX < -10000 && topLeftZ > 10000, "zoom 0 starts at the north-west corner");
        Assert.AreEqual(0.0, zoom1X, 50.0, "tile (1,1) starts at the centre ...");
        Assert.AreEqual(0.0, zoom1Z, 50.0);
        Assert.IsTrue(WorldMapFrame.PixelCenter(1, 1, 1, 255, 255).WorldZ < 0, "... and covers the south-east quarter");
        Assert.IsFalse(WorldMapFrame.IsValidTile(1, 2, 0));
        Assert.IsFalse(WorldMapFrame.IsValidTile(WorldMapFrame.MaxZoom + 1, 0, 0));
    }

    [TestMethod]
    public void ServiceServesVersionOneTilesAsPngWithAMimeType()
    {
        var service = new WorldMapService();
        Assert.AreEqual("map_not_ready", service.GetFrameTile("valheim", 0, 0, 0).ErrorCode);
        Assert.AreEqual("not_found", ProtocolHandshake.ToProtocolErrorCode("map_not_ready"), "Takaro retries a not_found tile");

        service.Publish(BuildRaster(64, 1, (_, _) => new WorldMapSample(WorldMapBiome.Meadows, 40, false)));
        var tile = service.GetFrameTile("valheim", 1, 0, 1);

        Assert.IsTrue(tile.Success);
        using var payload = JsonDocument.Parse(JsonSerializer.Serialize(tile.Payload));
        Assert.AreEqual("image/png", payload.RootElement.GetProperty("mimeType").GetString());
        var png = Convert.FromBase64String(payload.RootElement.GetProperty("data").GetString()!);
        var rgb = DecodeRgb(png, WorldMapFrame.TileSize, WorldMapFrame.TileSize);
        Assert.AreEqual(WorldMapFrame.TileSize * WorldMapFrame.TileSize * 3, rgb.Length);
        Assert.AreEqual("not_found", service.GetFrameTile("nether", 0, 0, 0).ErrorCode);
        Assert.AreEqual("invalid_args", service.GetFrameTile("valheim", 2, 4, 0).ErrorCode);
        Assert.AreNotSame(service.GetMapTile(0, 0, 0).Payload, service.GetFrameTile("valheim", 0, 0, 0).Payload, "the two layouts never share a cached tile");
    }

    private static WorldMapRaster BuildRaster(int size, long worldKey, Func<double, double, WorldMapSample> sample)
    {
        var builder = new WorldMapRasterBuilder(size, worldKey, (x, z) => sample(x, z));
        Assert.IsTrue(builder.Step(() => true));
        return builder.Raster;
    }

    private static byte[] DecodeRgb(byte[] png, int width, int height)
    {
        CollectionAssert.AreEqual(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }, png[..8]);
        var offset = 8;
        using var idat = new MemoryStream();
        while (offset < png.Length)
        {
            var length = (png[offset] << 24) | (png[offset + 1] << 16) | (png[offset + 2] << 8) | png[offset + 3];
            var type = System.Text.Encoding.ASCII.GetString(png, offset + 4, 4);
            var crc = (uint)((png[offset + 8 + length] << 24) | (png[offset + 9 + length] << 16)
                | (png[offset + 10 + length] << 8) | png[offset + 11 + length]);
            Assert.AreEqual(PngEncoder.Crc32(png, offset + 4, length + 4), crc, $"{type} CRC");
            if (type == "IHDR")
            {
                Assert.AreEqual(width, (png[offset + 8] << 24) | (png[offset + 9] << 16) | (png[offset + 10] << 8) | png[offset + 11]);
                Assert.AreEqual(2, png[offset + 17], "truecolour");
            }
            else if (type == "IDAT")
            {
                idat.Write(png, offset + 8, length);
            }

            offset += 12 + length;
        }

        using var zlib = new ZLibStream(new MemoryStream(idat.ToArray()), CompressionMode.Decompress);
        using var raw = new MemoryStream();
        zlib.CopyTo(raw);
        var scanlines = raw.ToArray();
        Assert.AreEqual((width * 3 + 1) * height, scanlines.Length);

        var rgb = new byte[width * height * 3];
        for (var row = 0; row < height; row++)
        {
            Assert.AreEqual(0, scanlines[row * (width * 3 + 1)], "filter none");
            Buffer.BlockCopy(scanlines, row * (width * 3 + 1) + 1, rgb, row * width * 3, width * 3);
        }

        return rgb;
    }
}
