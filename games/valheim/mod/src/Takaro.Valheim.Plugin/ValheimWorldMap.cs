using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using System.Diagnostics;
using UnityEngine;

namespace Takaro.Valheim.Plugin;

/// <summary>
/// Builds the world-map raster from the game's own <c>WorldGenerator</c>, the same source the
/// in-game minimap uses (<c>Minimap.GenerateWorldMap</c>), so the map matches what players see.
///
/// <c>WorldGenerator</c> keeps unsynchronised caches, so it is only ever sampled here, on the
/// Unity main thread, within a small per-frame budget. The finished raster is cached on disk
/// per world seed and generator version, so later restarts publish it at once.
/// </summary>
public sealed class ValheimWorldMap
{
    public const int RasterSize = 2048;

    private static readonly double FrameBudgetMilliseconds = 4.0;

    private readonly WorldMapService service;
    private readonly string cacheDirectory;
    private readonly Action<string> log;
    private WorldMapRasterBuilder? builder;
    private long builderWorldKey;
    private Stopwatch? buildClock;
    private float nextProgressLog;
    private bool failed;

    public ValheimWorldMap(WorldMapService service, string cacheDirectory, Action<string> log)
    {
        this.service = service;
        this.cacheDirectory = cacheDirectory;
        this.log = log;
    }

    /// <summary>Called from the plugin's Update on the Unity main thread.</summary>
    public void Update()
    {
        if (failed)
        {
            return;
        }

        try
        {
            Step();
        }
        catch (Exception exception)
        {
            failed = true;
            builder = null;
            service.SetStatus($"map rendering failed: {exception.Message}");
            log($"Takaro Valheim world map disabled after an error: {exception}");
        }
    }

    private void Step()
    {
        var generator = WorldGenerator.instance;
        var world = ZNet.World;
        if (generator is null || world is null || world.m_menu)
        {
            return;
        }

        var worldKey = WorldKey(world);
        var published = service.Raster;
        if (published is not null && published.WorldKey == worldKey)
        {
            return;
        }

        if (builder is null || builderWorldKey != worldKey)
        {
            if (TryLoadCached(worldKey))
            {
                return;
            }

            builder = new WorldMapRasterBuilder(RasterSize, worldKey, Sample);
            builderWorldKey = worldKey;
            buildClock = Stopwatch.StartNew();
            nextProgressLog = Time.realtimeSinceStartup + 10f;
            service.SetStatus("the world map is being rendered");
            log($"Takaro Valheim rendering the world map for seed {world.m_seed} ({RasterSize}x{RasterSize} samples, {FrameBudgetMilliseconds} ms per frame).");
        }

        var frame = Stopwatch.StartNew();
        var complete = builder.Step(() => frame.Elapsed.TotalMilliseconds < FrameBudgetMilliseconds);
        if (!complete)
        {
            service.SetStatus($"the world map is being rendered ({builder.Progress:P0})");
            if (Time.realtimeSinceStartup >= nextProgressLog)
            {
                nextProgressLog = Time.realtimeSinceStartup + 10f;
                log($"Takaro Valheim world map {builder.Progress:P0} rendered.");
            }

            return;
        }

        var raster = builder.Raster;
        builder = null;
        service.Publish(raster);
        log($"Takaro Valheim world map ready after {buildClock?.Elapsed.TotalSeconds:F1} s.");
        SaveCached(raster);
    }

    private static WorldMapSample Sample(float wx, float wz)
    {
        var generator = WorldGenerator.instance;
        var biome = generator.GetBiome(wx, wz);
        var height = generator.GetBiomeHeight(biome, wx, wz, out _);
        return new WorldMapSample(MapBiome(biome), height, IsForest(biome, wx, wz));
    }

    private static bool IsForest(Heightmap.Biome biome, float wx, float wz)
    {
        var position = new Vector3(wx, 0f, wz);
        return biome switch
        {
            Heightmap.Biome.BlackForest => true,
            Heightmap.Biome.Meadows => WorldGenerator.InForest(position),
            Heightmap.Biome.Plains => WorldGenerator.GetForestFactor(position) < 0.8f,
            Heightmap.Biome.Mistlands => WorldGenerator.GetForestFactor(position) < 1.1f,
            _ => false
        };
    }

    private static WorldMapBiome MapBiome(Heightmap.Biome biome) => biome switch
    {
        Heightmap.Biome.Meadows => WorldMapBiome.Meadows,
        Heightmap.Biome.Swamp => WorldMapBiome.Swamp,
        Heightmap.Biome.Mountain => WorldMapBiome.Mountain,
        Heightmap.Biome.BlackForest => WorldMapBiome.BlackForest,
        Heightmap.Biome.Plains => WorldMapBiome.Plains,
        Heightmap.Biome.AshLands => WorldMapBiome.AshLands,
        Heightmap.Biome.DeepNorth => WorldMapBiome.DeepNorth,
        Heightmap.Biome.Ocean => WorldMapBiome.Ocean,
        Heightmap.Biome.Mistlands => WorldMapBiome.Mistlands,
        _ => WorldMapBiome.Unknown
    };

    private static long WorldKey(World world) =>
        ((long)world.m_seed << 16) ^ (uint)world.m_worldGenVersion;

    private string CachePath(long worldKey) =>
        Path.Combine(cacheDirectory, $"takaro-valheim-worldmap-{worldKey:x16}-{RasterSize}.bin");

    private bool TryLoadCached(long worldKey)
    {
        var path = CachePath(worldKey);
        try
        {
            if (!File.Exists(path))
            {
                return false;
            }

            var raster = WorldMapRaster.TryDeserialize(File.ReadAllBytes(path), RasterSize, worldKey);
            if (raster is null)
            {
                log($"Takaro Valheim ignoring an invalid world map cache at {path}.");
                return false;
            }

            service.Publish(raster);
            log($"Takaro Valheim world map loaded from cache {path}.");
            return true;
        }
        catch (Exception exception)
        {
            log($"Takaro Valheim could not read the world map cache: {exception.Message}");
            return false;
        }
    }

    private void SaveCached(WorldMapRaster raster)
    {
        var path = CachePath(raster.WorldKey);
        _ = Task.Run(() =>
        {
            try
            {
                Directory.CreateDirectory(cacheDirectory);
                var temporary = path + ".tmp";
                File.WriteAllBytes(temporary, raster.Serialize());
                if (File.Exists(path))
                {
                    File.Delete(path);
                }

                File.Move(temporary, path);
                log($"Takaro Valheim world map cached at {path}.");
            }
            catch (Exception exception)
            {
                log($"Takaro Valheim could not cache the world map: {exception.Message}");
            }
        });
    }
}
#endif
