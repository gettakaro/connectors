using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Eleon.Modding;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Core;
using TakaroEmpyrion.Game;
using TakaroEmpyrion.Map;
using TakaroEmpyrion.Protocol;

/// <summary>Protocol v1 handshake and the planet map: file formats, frame, tiles, PNG encoding, wire.</summary>
public static partial class ContractTests
{
    // A synthetic save laid out like the real one: Saves/Cache/<game>/Playfields/<pf> + Saves/Games/<game>/Templates/<pf>.
    sealed class FakeSave : IDisposable
    {
        public readonly string Root = Path.Combine(Path.GetTempPath(), "takaro-emp-map-" + Guid.NewGuid().ToString("N"));
        public string Playfields { get { return Path.Combine(Root, "Cache", "Game", "Playfields"); } }
        public string SaveGame { get { return Path.Combine(Root, "Games", "Game"); } }
        public string Templates { get { return Path.Combine(SaveGame, "Templates"); } }

        public const float Ground = 50f;

        /// <summary>A flat 1024 x 512 planet of Grassland at 50 m with a 4 x 4 DeepWater patch around world (12, 396).</summary>
        public void AddPlanet(string name, int width = 1024, string yaml = "PlayfieldType: Moon\nSeaLevel: 30\n", bool biomes = true)
        {
            string pf = Path.Combine(Playfields, name), tpl = Path.Combine(Templates, name);
            Directory.CreateDirectory(pf);
            Directory.CreateDirectory(tpl);
            int height = width / 2;
            var raw = new byte[width * height * 2];
            ushort v = (ushort)Math.Round(Ground / 490.0 * 65535);
            for (int i = 0; i < width * height; i++) { raw[2 * i] = (byte)(v & 0xFF); raw[2 * i + 1] = (byte)(v >> 8); }
            File.WriteAllBytes(Path.Combine(pf, "terrain.raw"), raw);
            if (biomes)
            {
                var cells = Enumerable.Repeat((byte)2, width * height).ToArray();
                for (int r = 304; r <= 307; r++) for (int c = 512; c <= 515; c++) cells[r * width + c] = 1;
                File.WriteAllBytes(Path.Combine(pf, "biomes.dat"), BiomesDat(new[] { "DeepWater", "Grassland" }, cells));
            }
            File.WriteAllText(Path.Combine(tpl, "playfield.yaml"), yaml);
            File.WriteAllText(Path.Combine(tpl, "terrain.ecf"), "{ Material Name: 1 Test\n  HeightMapMax: \"490\"\n}\n");
        }

        public void Dispose() { try { Directory.Delete(Root, true); } catch (Exception) { } }
    }

    /// <summary>biomes.dat in the layout measured on the v1.19 dedi (ids 1..n in name order).</summary>
    static byte[] BiomesDat(string[] names, byte[] cells)
    {
        var ms = new MemoryStream();
        ms.Write(new byte[] { 4, 0, 4, 0, 2 }, 0, 5);
        ms.WriteByte((byte)names.Length); ms.WriteByte(0);
        for (int i = 0; i < names.Length; i++)
        {
            ms.WriteByte((byte)names[i].Length);
            ms.Write(Encoding.ASCII.GetBytes(names[i]), 0, names[i].Length);
            ms.WriteByte((byte)(i + 1)); ms.WriteByte(0);
        }
        var rle = new List<byte>();
        for (int i = 0; i < cells.Length;)
        {
            int run = 1;
            while (i + run < cells.Length && cells[i + run] == cells[i] && run < 255) run++;
            rle.Add((byte)run); rle.Add(cells[i]);
            i += run;
        }
        ms.WriteByte(1);
        ms.Write(BitConverter.GetBytes(rle.Count), 0, 4);
        ms.Write(rle.ToArray(), 0, rle.Count);
        ms.Write(new byte[10], 0, 10); // the trailing section the connector does not read
        return ms.ToArray();
    }

    sealed class NullLog : ILog
    {
        public void Debug(string s) { }
        public void Info(string s) { }
        public void Warn(string s) { }
        public void Error(string s) { }
    }

    /// <summary>Takaro's mapTileGameRect (lib-gameserver generic/map.ts), ported to check the frame from Takaro's side.</summary>
    static void TakaroTileRect(JObject dim, int zoom, int x, int y, out double hMin, out double hMax, out double vMin, out double vMax)
    {
        double bhMin = (double)dim["bounds"]["horizontal"]["min"], bhMax = (double)dim["bounds"]["horizontal"]["max"];
        double bvMin = (double)dim["bounds"]["vertical"]["min"], bvMax = (double)dim["bounds"]["vertical"]["max"];
        int hSign = (string)dim["axes"]["horizontal"]["direction"] == "right" ? 1 : -1;
        int vSign = (string)dim["axes"]["vertical"]["direction"] == "up" ? 1 : -1;
        double minX = hSign == 1 ? bhMin : -bhMax, maxX = hSign == 1 ? bhMax : -bhMin;
        double minY = vSign == 1 ? bvMin : -bvMax, maxY = vSign == 1 ? bvMax : -bvMin;
        double side = Math.Max(maxX - minX, maxY - minY) / Math.Pow(2, zoom);
        double px0 = minX + x * side, px1 = minX + (x + 1) * side;
        double py0 = maxY - (y + 1) * side, py1 = maxY - y * side;
        if (hSign == 1) { hMin = px0; hMax = px1; } else { hMin = -px1; hMax = -px0; }
        if (vSign == 1) { vMin = py0; vMax = py1; } else { vMin = -py1; vMax = -py0; }
    }

    /// <summary>Decodes the PNGs this connector writes (8-bit RGBA, filters 0/1), checking signature and every CRC.</summary>
    static byte[] DecodePng(byte[] png, out int w, out int h, out string problem)
    {
        w = h = 0; problem = null;
        byte[] sig = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        if (png == null || png.Length < 8 || !png.Take(8).SequenceEqual(sig)) { problem = "bad signature"; return null; }
        var idat = new MemoryStream();
        int o = 8;
        bool end = false;
        while (o + 12 <= png.Length)
        {
            int len = (png[o] << 24) | (png[o + 1] << 16) | (png[o + 2] << 8) | png[o + 3];
            string type = Encoding.ASCII.GetString(png, o + 4, 4);
            uint crc = (uint)((png[o + 8 + len] << 24) | (png[o + 9 + len] << 16) | (png[o + 10 + len] << 8) | png[o + 11 + len]);
            if (Png.Crc32(png, o + 4, len + 4) != crc) { problem = "bad CRC in " + type; return null; }
            if (type == "IHDR")
            {
                w = (png[o + 8] << 24) | (png[o + 9] << 16) | (png[o + 10] << 8) | png[o + 11];
                h = (png[o + 12] << 24) | (png[o + 13] << 16) | (png[o + 14] << 8) | png[o + 15];
                if (png[o + 16] != 8 || png[o + 17] != 6) { problem = "not 8-bit RGBA"; return null; }
            }
            else if (type == "IDAT") idat.Write(png, o + 8, len);
            else if (type == "IEND") { end = true; break; }
            o += 12 + len;
        }
        if (!end) { problem = "no IEND"; return null; }
        byte[] z = idat.ToArray();
        if (z.Length < 6 || z[0] != 0x78) { problem = "bad zlib header"; return null; }
        var raw = new MemoryStream();
        using (var d = new DeflateStream(new MemoryStream(z, 2, z.Length - 6), CompressionMode.Decompress)) d.CopyTo(raw);
        byte[] f = raw.ToArray();
        int stride = w * 4;
        if (f.Length != (stride + 1) * h) { problem = "inflated " + f.Length + " bytes, expected " + (stride + 1) * h; return null; }
        var rgba = new byte[stride * h];
        for (int y = 0; y < h; y++)
        {
            byte filter = f[y * (stride + 1)];
            for (int i = 0; i < stride; i++)
            {
                int v = f[y * (stride + 1) + 1 + i];
                if (filter == 1 && i >= 4) v += rgba[y * stride + i - 4];
                else if (filter > 1) { problem = "unexpected filter " + filter; return null; }
                rgba[y * stride + i] = (byte)v;
            }
        }
        return rgba;
    }

    static bool IsBlue(byte[] rgba, int w, int x, int y) { int o = (y * w + x) * 4; return rgba[o + 2] > rgba[o + 1] + 20 && rgba[o + 2] > rgba[o] + 40; }
    static bool IsGreen(byte[] rgba, int w, int x, int y) { int o = (y * w + x) * 4; return rgba[o + 1] > rgba[o + 2] + 40; }

    // ------------------------------------------------------------------ files + frame

    static void MapFrame()
    {
        Check(Planet.DimensionId("Akua") == "Akua" && Planet.DimensionId("Ashon II Moon") == "Ashon_II_Moon" && Planet.DimensionId("+X") == "p_X",
            "dimension id: names kept, spaces and symbols made Takaro-safe");

        using (var save = new FakeSave())
        {
            save.AddPlanet("Test Planet");
            save.AddPlanet("Big", 1024, "PlayfieldType: Planet\nPlanetSize: 3\n");
            Directory.CreateDirectory(Path.Combine(save.Playfields, "Orbit Sector")); // space: no terrain

            Check(MapService.FindPlayfieldsDir(Path.Combine(save.Root, "Cache"), save.SaveGame) == save.Playfields, "playfields dir from AppFolder.Cache = cache root");
            Check(MapService.FindPlayfieldsDir(null, save.SaveGame) == save.Playfields, "playfields dir derived from the save folder");

            Planet p = Planet.Probe(Path.Combine(save.Playfields, "Test Planet"), save.Templates, out string why);
            Check(p != null && p.WidthPx == 1024 && p.HeightPx == 512 && p.PlanetSize == 2 && p.WorldWidth == 8192 && p.WorldHeight == 4096
                  && p.MetresPerPixel == 8 && p.HeightMapMax == 490 && p.SeaLevel == 30 && p.NativeZoom == 2 && p.MaxZoom == 4,
                "terrain.raw 1048576 bytes -> 1024x512; absent PlanetSize = 2 -> 8192x4096 m, 8 m/px, zoom 0-4", why ?? "");
            Planet big = Planet.Probe(Path.Combine(save.Playfields, "Big"), save.Templates, out why);
            Check(big != null && big.WorldWidth == 16384 && big.MetresPerPixel == 16, "PlanetSize 3 doubles the planet (8192 * 2^(size-2))");

            JObject dim = p.ToDimension();
            var expected = JObject.Parse(@"{""id"":""Test_Planet"",""name"":""Test Planet"",
                ""axes"":{""horizontal"":{""field"":""x"",""direction"":""right""},""vertical"":{""field"":""z"",""direction"":""up""}},
                ""bounds"":{""horizontal"":{""min"":-4096.0,""max"":4096.0},""vertical"":{""min"":-2048.0,""max"":2048.0}},
                ""source"":{""type"":""tiled"",""tileSize"":256,""minZoom"":0,""maxZoom"":4}}");
            Check(JToken.DeepEquals(dim, expected), "dimension manifest entry: x right, z up, whole-planet bounds, tiled 256 zoom 0-4", dim.ToString(Newtonsoft.Json.Formatting.None));

            // Akua calibration point: a logged player at (12, 396) stands on heightmap column 513.5, row 305.5.
            p.WorldToRaster(12, 396, out double col, out double row);
            Check(col == 513.5 && row == 305.5, "world (12, 396) -> heightmap (513.5, 305.5)");
            p.WorldToTile(12, 396, 2, out int tx, out int ty, out double px, out double py);
            Check(tx == 2 && ty == 0 && Math.Abs(px - 1.5) < 1e-9 && Math.Abs(py - 206.5) < 1e-9, "world (12, 396) -> zoom 2 tile 2/0 pixel (1.5, 206.5)");
            bool agrees = true;
            foreach (var pt in new[] { new[] { 12.0, 396 }, new[] { -4000.0, 2000 }, new[] { 4095.0, -2047 }, new[] { 282.3, 784.57 }, new[] { 0.0, 0 } })
                for (int zoom = 0; zoom <= p.MaxZoom; zoom++)
                {
                    p.WorldToTile(pt[0], pt[1], zoom, out tx, out ty, out px, out py);
                    TakaroTileRect(dim, zoom, tx, ty, out double hMin, out double hMax, out double vMin, out double vMax);
                    if (!(pt[0] >= hMin && pt[0] < hMax && pt[1] > vMin && pt[1] <= vMax)) agrees = false;
                }
            Check(agrees, "our tile for a world point is the tile Takaro's frame puts it in, every zoom");

            File.WriteAllBytes(Path.Combine(save.Playfields, "Test Planet", "terrain.raw"), new byte[1000]);
            Check(Planet.Probe(Path.Combine(save.Playfields, "Test Planet"), save.Templates, out why) == null && why.Contains("not a W x W/2"), "odd-sized terrain.raw refused");
            Directory.CreateDirectory(Path.Combine(save.Playfields, "NoTemplate"));
            File.WriteAllBytes(Path.Combine(save.Playfields, "NoTemplate", "terrain.raw"), new byte[1024 * 512 * 2]);
            Check(Planet.Probe(Path.Combine(save.Playfields, "NoTemplate"), save.Templates, out why) == null && why.Contains("planet size unknown"), "no template -> no map (size unknown), not a guess");
        }

        var cells = new byte[] { 3, 3, 3, 1, 1, 2 };
        BiomeMap bm = BiomeMap.Parse(BiomesDat(new[] { "Water", "Sand", "Grass" }, cells), cells.Length);
        Check(bm != null && bm.Names[1] == "Water" && bm.Names[3] == "Grass" && bm.Cells.SequenceEqual(cells), "biomes.dat: names, ids and RLE grid");
        Check(BiomeMap.Parse(BiomesDat(new[] { "Water" }, new byte[] { 1, 1 }), 3) == null, "biomes.dat with the wrong cell count is ignored");
        Renderer.BiomeColour("ShallowWaterLilies", out bool water);
        Check(water && Renderer.BiomeColour("PolarSnow", out water)[0] > 200 && !water, "biome names coloured by keyword (water, snow)");
        Check(Renderer.BiomeColour("Crashsite", out water)[0] == 110 && Renderer.BiomeColour("Forest_1", out water)[1] == 92,
            "keyword order: Crashsite is not lava ('ash'), Forest is not ore");
    }

    // ------------------------------------------------------------------ tiles

    static void MapTiles()
    {
        using (var save = new FakeSave())
        {
            save.AddPlanet("Test Planet");
            var map = new MapService(() => save.Playfields, () => save.Templates, new NullLog());
            Check(map.Rescan() && !map.Rescan(), "rescan reports a change once");
            JObject cap = map.Capability();
            Check(cap != null && (string)cap["name"] == "map" && cap["parameters"]["dimensions"].Count() == 1
                  && (string)cap["parameters"]["dimensions"][0]["id"] == "Test_Planet", "map capability with one Dimension per planet");

            byte[] png = map.Tile("Test_Planet", 2, 2, 0);
            byte[] rgba = DecodePng(png, out int w, out int h, out string problem);
            Check(rgba != null && w == 256 && h == 256, "tile is a valid 256x256 RGBA PNG (signature, CRCs, zlib)", problem ?? "");
            byte[] direct = map.RendererFor(map.Find("Test_Planet")).RenderTile(2, 2, 0, 256);
            Check(rgba != null && rgba.SequenceEqual(direct), "PNG round-trips the rendered pixels exactly");
            Check(png.Length < 64 * 1024, "flat tile compresses (" + png.Length + " bytes)");
            Check(ReferenceEquals(png, map.Tile("Test_Planet", 2, 2, 0)), "tiles are cached");

            // The DeepWater patch at world (12, 396) must land at pixel (1.5, 206.5) of tile 2/2/0: north up, x right.
            Check(rgba != null && IsBlue(rgba, 256, 1, 206) && IsBlue(rgba, 256, 2, 207) && IsGreen(rgba, 256, 20, 206) && IsGreen(rgba, 256, 1, 180),
                "marker at world (12, 396) is drawn at the pixel the frame maps it to");
            byte[] south = DecodePng(map.Tile("Test_Planet", 2, 2, 1), out w, out h, out problem);
            Check(south != null && IsGreen(south, 256, 1, 49), "no marker at the vertically mirrored spot (north is up)");
            byte[] z4 = DecodePng(map.Tile("Test_Planet", 4, 8, 3), out w, out h, out problem);
            map.Find("Test_Planet").WorldToTile(12, 396, 4, out int t4x, out int t4y, out double p4x, out double p4y);
            Check(z4 != null && t4x == 8 && t4y == 3 && IsBlue(z4, 256, (int)p4x, (int)p4y), "marker also right at the deepest zoom (" + (int)p4x + "," + (int)p4y + ")");

            byte[] z0 = DecodePng(map.Tile("Test_Planet", 0, 0, 0), out w, out h, out problem);
            Check(z0 != null && z0[(10 * 256 + 10) * 4 + 3] == 255 && z0[(200 * 256 + 10) * 4 + 3] == 0, "zoom 0: planet in the top half, transparent below");
            Check(map.Tile("Test_Planet", 1, 0, 1) == null && map.Tile("Test_Planet", 2, 3, 3) == null, "tiles south of the planet are empty");
            bool notFound = false, outOfRange = false;
            try { map.Tile("Nowhere", 0, 0, 0); } catch (MapNotFoundException) { notFound = true; }
            try { map.Tile("Test_Planet", 5, 0, 0); } catch (ArgumentOutOfRangeException) { outOfRange = true; }
            Check(notFound && outOfRange, "unknown dimension and zoom past maxZoom refused");

            var actions = new Actions(new GameBridge(new NullLog()), new FakeV2(), new PlayerRegistry(null), new Catalog(), null, new Config(), new NullLog(), new BanReasons(null), _ => { }, map);
            JToken res = actions.Run("getMapTile", Args.Parse(new JValue("{\"dimension\":\"Test_Planet\",\"zoom\":2,\"x\":2,\"y\":0}"))).Result;
            Check((string)res["mimeType"] == "image/png" && Convert.FromBase64String((string)res["data"]).SequenceEqual(png), "getMapTile answers {mimeType, data} with the base64 PNG");
            Check(CodeOf(() => actions.Run("getMapTile", Args.Parse(new JValue("{\"dimension\":\"Test_Planet\",\"zoom\":1,\"x\":0,\"y\":1}"))).Wait()) == ErrorCodes.NotFound, "outside the planet -> not_found (Takaro draws it transparent)");
            Check(CodeOf(() => actions.Run("getMapTile", Args.Parse(new JValue("{\"dimension\":\"Elsewhere\",\"zoom\":0,\"x\":0,\"y\":0}"))).Wait()) == ErrorCodes.NotFound, "unknown dimension -> not_found");
            Check(CodeOf(() => actions.Run("getMapTile", Args.Parse(new JValue("{\"x\":1,\"y\":2,\"z\":3}"))).Wait()) == ErrorCodes.InvalidArgs, "v0-shaped tile args -> invalid_args");
            Check(CodeOf(() => actions.Run("getMapInfo", Args.Parse(null)).Wait()) == ErrorCodes.Unsupported, "getMapInfo (v0 only) -> unsupported");

            save.AddPlanet("Second");
            Check(map.Rescan() && map.Capability()["parameters"]["dimensions"].Count() == 2, "a planet whose terrain appears later is added on rescan");
        }

        using (var save = new FakeSave())
        {
            save.AddPlanet("Bare", 1024, "SeaLevel: 30\n", biomes: false);
            var map = new MapService(() => save.Playfields, () => save.Templates, new NullLog());
            map.Rescan();
            Check(DecodePng(map.Tile("Bare", 2, 1, 1), out int w, out int h, out string problem) != null, "planet without biomes.dat still renders (height colours)", problem ?? "");
        }
    }

    static string CodeOf(Action a)
    {
        try { a(); return null; }
        catch (Exception e) { Actions.Classify(e, out string code, out string _); return code; }
    }

    // ------------------------------------------------------------------ real save (EMPYRION_SAVES)

    static void MapRealData()
    {
        string saves = Environment.GetEnvironmentVariable("EMPYRION_SAVES");
        if (string.IsNullOrEmpty(saves) || !Directory.Exists(Path.Combine(saves, "Cache")))
        {
            Console.WriteLine("SKIP map real data (set EMPYRION_SAVES=<server>/Saves)");
            return;
        }
        string game = Environment.GetEnvironmentVariable("EMPYRION_GAME");
        if (string.IsNullOrEmpty(game)) game = Path.GetFileName(Directory.GetDirectories(Path.Combine(saves, "Cache")).First(d => Directory.Exists(Path.Combine(d, "Playfields"))));
        string saveGame = Path.Combine(saves, "Games", game);
        var map = new MapService(() => MapService.FindPlayfieldsDir(null, saveGame), () => Path.Combine(saveGame, "Templates"), new NullLog());
        map.Rescan();
        Console.WriteLine("     real save '" + game + "': " + string.Join(", ", map.Planets.Select(p => p.Name + " " + p.WidthPx + "x" + p.HeightPx + "px size " + p.PlanetSize)));
        Check(map.Planets.Count > 0, "real save: at least one planet with terrain");

        Planet akua = map.Find("Akua");
        if (akua != null)
        {
            Check(akua.WidthPx == 1024 && akua.PlanetSize == 2 && akua.WorldWidth == 8192 && akua.HeightMapMax == 490, "real Akua: 1024x512, size 2, 8192 m, HeightMapMax 490");
            // getPlayerLocation answers logged by the connector on Akua (takaro.log, 2026-11-03): x, y, z.
            double[][] logged =
            {
                new[] { 11.997814178466797, 50.472434997558594, 396.0 }, new[] { 282.3070068359375, 45.283176422119141, 784.570068359375 },
                new[] { 234.5015869140625, 47.449905395507813, 860.49957275390625 }, new[] { 93.514755249023438, 50.208229064941406, 507.06097412109375 },
                new[] { -48.5, 49.3849983215332, 375.5 }, new[] { 158.62948608398438, 49.290523529052734, 623.3175048828125 },
                new[] { 188.77874755859375, 50.041366577148438, 686.3856201171875 }, new[] { 233.69219970703125, 47.515052795410156, 770.433837890625 },
            };
            float[] hgt = akua.ReadHeights();
            double worst = 0;
            foreach (double[] l in logged)
            {
                akua.WorldToRaster(l[0], l[2], out double c, out double r);
                int c0 = (int)Math.Floor(c), r0 = (int)Math.Floor(r);
                double fc = c - c0, fr = r - r0;
                double ground = hgt[r0 * 1024 + c0] * (1 - fc) * (1 - fr) + hgt[r0 * 1024 + c0 + 1] * fc * (1 - fr)
                                + hgt[(r0 + 1) * 1024 + c0] * (1 - fc) * fr + hgt[(r0 + 1) * 1024 + c0 + 1] * fc * fr;
                worst = Math.Max(worst, Math.Abs(l[1] - 0.5 - ground));
            }
            Check(worst < 0.15, "real Akua: 8 logged player positions stand 0.5 m above the terrain under the mapped pixel (worst " + worst.ToString("0.000") + " m)");
        }

        foreach (Planet p in map.Planets)
        {
            // The whole planet at native zoom, cut through the same tile path Takaro calls, stitched into one image.
            int n = 1 << p.NativeZoom, W = p.WidthPx, H = p.HeightPx;
            var full = new byte[W * H * 4];
            for (int ty = 0; ty < n / 2; ty++)
                for (int tx = 0; tx < n; tx++)
                {
                    byte[] t = DecodePng(map.Tile(p.Id, p.NativeZoom, tx, ty), out int w, out int h, out string problem);
                    if (t == null) { Check(false, "real " + p.Name + " tile " + tx + "/" + ty, problem ?? "empty"); continue; }
                    for (int y = 0; y < 256; y++) Buffer.BlockCopy(t, y * 1024, full, ((ty * 256 + y) * W + tx * 256) * 4, 1024);
                }
            string outPath = Path.Combine("bin", "tests", "map-" + p.Id + ".png");
            File.WriteAllBytes(outPath, Png.EncodeRgba(full, W, H));
            Console.WriteLine("     wrote " + outPath);
        }
    }

    // ------------------------------------------------------------------ wire v1

    static void WireV1()
    {
        using (var save = new FakeSave())
        {
            save.AddPlanet("Test Planet");
            var map = new MapService(() => save.Playfields, () => save.Templates, new NullLog());
            map.Rescan();

            var log = new ConsoleLog();
            var bridge = new GameBridge(log);
            var game = new FakeGame { Bridge = bridge };
            game.Players[101] = new PlayerInfo { entityId = 101, steamId = "76561198000000001", playerName = "Tester", playfield = "Test Planet", pos = new PVector3(12, 50.5f, 396), ping = 42 };
            bridge.Attach(game);
            var ticker = new Thread(() => { while (true) { bridge.Tick(); Thread.Sleep(2); } }) { IsBackground = true };
            ticker.Start();
            var players = new PlayerRegistry(null);
            var actions = new Actions(bridge, new FakeV2(), players, new Catalog(), null, new Config { ServerName = "Dev", IdentityToken = "id" }, log, new BanReasons(null), _ => { }, map);

            var sockets = new BlockingCollection<FakeSocket>();
            TakaroClient client = null;
            client = new TakaroClient(() => { var s = new FakeSocket(); sockets.Add(s); return s; },
                new ClientOptions
                {
                    Url = "wss://fake/", IdentityToken = "id", RegistrationToken = "reg", ServerName = "Dev",
                    PingInterval = TimeSpan.FromSeconds(30), InboundTimeout = TimeSpan.FromSeconds(60),
                    Manifest = () => Capabilities.Build(Actions.Declared, map.Capability()),
                },
                log,
                req => Task.Run(async () =>
                {
                    try { client.SendResponse(req.RequestId, await actions.Run(req.Action, Args.Parse(req.RawArgs))); }
                    catch (Exception e) { Actions.Classify(e, out string code, out string msg); client.SendError(req.RequestId, msg, code); }
                }));
            client.Start();

            FakeSocket sock = sockets.Take();
            JObject ident = Next(sock, o => (string)o["type"] == "identify");
            JToken ip = ident?["payload"];
            var names = (ip?["capabilities"] as JArray)?.Select(c => (string)c["name"]).ToList() ?? new List<string>();
            Check(ip != null && ip["protocolVersions"].ToObject<int[]>().SequenceEqual(new[] { 0, 1 }), "V1 identify offers protocol versions [0, 1]");
            Check(names.Contains("players.list") && names.Contains("players.location") && names.Contains("moderation.listBans") && names.Contains("events.chat-message")
                  && !names.Contains("locations.list") && names.Count == names.Distinct().Count(), "V1 manifest: handled actions + all events, no locations.list, no duplicates");
            JToken mapCap = (ip?["capabilities"] as JArray)?.FirstOrDefault(c => (string)c["name"] == "map");
            Check(mapCap != null && (string)mapCap["parameters"]["dimensions"][0]["id"] == "Test_Planet" && (string)mapCap["parameters"]["dimensions"][0]["source"]["type"] == "tiled",
                "V1 manifest declares the map with the planet Dimension");

            sock.ToConnector.Add("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs1\",\"protocolVersion\":1}}");
            DateTime until = DateTime.UtcNow.AddSeconds(3);
            while (client.ProtocolVersion != 1 && DateTime.UtcNow < until) Thread.Sleep(10);
            Check(client.ProtocolVersion == 1, "V1 negotiated when Takaro answers protocolVersion 1");

            client.SendGameEvent("log", new JObject { ["msg"] = "one" });
            client.SendGameEvent("log", new JObject { ["msg"] = "two" });
            JObject e1 = Next(sock, o => (string)o["type"] == "gameEvent"), e2 = Next(sock, o => (string)o["type"] == "gameEvent");
            Check(e1 != null && e2 != null && (long)e1["payload"]["seq"] == 1 && (long)e2["payload"]["seq"] == 2 && (string)e1["payload"]["streamId"] == client.StreamId
                  && (string)e1["payload"]["type"] == "log" && (string)e1["payload"]["data"]["msg"] == "one", "V1 events carry streamId and an increasing seq");
            sock.ToConnector.Add(new JObject { ["type"] = "eventAck", ["payload"] = new JObject { ["streamId"] = "someone-else", ["upTo"] = 2 } }.ToString());
            sock.ToConnector.Add(new JObject { ["type"] = "eventAck", ["payload"] = new JObject { ["streamId"] = client.StreamId, ["upTo"] = 1 } }.ToString());
            until = DateTime.UtcNow.AddSeconds(3);
            while (client.UnackedEvents != 1 && DateTime.UtcNow < until) Thread.Sleep(10);
            Check(client.UnackedEvents == 1, "V1 eventAck releases up to seq, ignores another stream");

            Func<string, JToken, JObject> ask = (action, args) =>
            {
                string rid = Guid.NewGuid().ToString();
                sock.ToConnector.Add(new JObject { ["type"] = "request", ["requestId"] = rid, ["payload"] = new JObject { ["action"] = action, ["args"] = args } }.ToString());
                return Next(sock, o => (string)o["requestId"] == rid);
            };
            JObject r = ask("getPlayerLocation", "{\"gameId\":\"76561198000000001\"}");
            Check(r != null && (string)r["payload"]["dimension"] == "Test_Planet" && (double)r["payload"]["x"] == 12, "V1 getPlayerLocation carries the Dimension id of the playfield");

            r = ask("getMapTile", "{\"dimension\":\"Test_Planet\",\"zoom\":2,\"x\":2,\"y\":0}");
            byte[] tile = r?["payload"]?["data"] == null ? null : Convert.FromBase64String((string)r["payload"]["data"]);
            Check(r != null && (string)r["type"] == "response" && (string)r["payload"]["mimeType"] == "image/png"
                  && DecodePng(tile, out int tw, out int th, out string _) != null && tw == 256, "V1 getMapTile over the wire: {mimeType, data} with a 256 px PNG");
            r = ask("getMapTile", "{\"dimension\":\"Test_Planet\",\"zoom\":1,\"x\":1,\"y\":1}");
            Check(r != null && (string)r["type"] == "response" && (string)r["error"]?["code"] == "not_found" && r["payload"].Type == JTokenType.Null,
                "V1 tile outside the planet: response with error {code: not_found}");
            r = ask("getMapInfo", "{}");
            Check(r != null && (string)r["type"] == "response" && (string)r["error"]?["code"] == "unsupported" && ((string)r["error"]["message"]).Contains("getMapInfo"),
                "V1 unknown action: error code unsupported");

            Check(client.SendCapabilities() && Next(sock, o => (string)o["type"] == "capabilities" && o["payload"]["capabilities"] is JArray) != null,
                "V1 manifest can be replaced (capabilities frame)");

            // Reconnect: the unacknowledged event comes back with its own seq; new ones continue the sequence.
            sock.ToConnector.Add("<close>");
            FakeSocket sock2;
            Check(sockets.TryTake(out sock2, 8000), "V1 reconnects after close");
            if (sock2 != null)
            {
                Next(sock2, o => (string)o["type"] == "identify");
                sock2.ToConnector.Add("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs1\",\"protocolVersion\":1}}");
                JObject again = Next(sock2, o => (string)o["type"] == "gameEvent");
                client.SendGameEvent("log", new JObject { ["msg"] = "three" });
                JObject e3 = Next(sock2, o => (string)o["type"] == "gameEvent");
                Check(again != null && (long)again["payload"]["seq"] == 2 && (string)again["payload"]["data"]["msg"] == "two"
                      && e3 != null && (long)e3["payload"]["seq"] == 3, "V1 resends unacknowledged events after reconnect, seq keeps growing");

                // An older Takaro that names no version keeps the connection on v0.
                sock2.ToConnector.Add("<close>");
                FakeSocket sock3;
                if (sockets.TryTake(out sock3, 8000))
                {
                    Next(sock3, o => (string)o["type"] == "identify");
                    sock3.ToConnector.Add("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs1\"}}");
                    JObject old = Next(sock3, o => (string)o["type"] == "gameEvent");
                    Check(client.ProtocolVersion == 0 && old != null && old["payload"]["seq"] == null && client.UnackedEvents == 0,
                        "older Takaro (no protocolVersion): v0 events without seq");
                    string rid = Guid.NewGuid().ToString();
                    sock3.ToConnector.Add(new JObject { ["type"] = "request", ["requestId"] = rid, ["payload"] = new JObject { ["action"] = "getMapInfo", ["args"] = "{}" } }.ToString());
                    JObject v0err = Next(sock3, o => (string)o["requestId"] == rid);
                    Check(v0err != null && (string)v0err["type"] == "error" && v0err["payload"]["error"] != null, "v0 errors keep the error frame");
                }
                else Check(false, "reconnect for the v0 fallback");
            }

            client.Stop();
        }

        var opts = new ClientOptions { Url = "wss://fake/", IdentityToken = "id", ServerName = "Dev" };
        var plain = new BlockingCollection<FakeSocket>();
        var log0 = new ConsoleLog();
        var c0 = new TakaroClient(() => { var s = new FakeSocket(); plain.Add(s); return s; }, opts, log0, _ => { });
        c0.Start();
        FakeSocket s0 = plain.Take();
        JObject id0 = Next(s0, o => (string)o["type"] == "identify");
        Check(id0 != null && id0["payload"]["protocolVersions"] == null && id0["payload"]["capabilities"] == null, "PROTOCOL_VERSION=0 (no manifest): identify stays v0");
        // A v1 refusal has payload null and a top-level error.
        s0.ToConnector.Add("{\"type\":\"identifyResponse\",\"payload\":null,\"error\":{\"code\":\"invalid_args\",\"message\":\"bad manifest\"}}");
        DateTime end = DateTime.UtcNow.AddSeconds(3);
        bool refused = false;
        while (!refused && DateTime.UtcNow < end) { lock (log0.Lines) refused = log0.Lines.Any(l => l.Contains("rejected identify: bad manifest")); Thread.Sleep(10); }
        Check(refused && !c0.Identified, "v1 identify refusal (top-level error) is reported, not taken as success");
        c0.Stop();
    }
}
