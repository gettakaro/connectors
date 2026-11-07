// takaro-icons-decoder-unreal: reads Unreal Engine game files (IoStore and pak) through CUE4Parse and
// writes what an icon build asks for: DataTables and other packages as JSON, textures as PNG, and the
// AssetRegistry as a trimmed JSON listing.
//
// The tool runs without network access. It speaks the decoder-container contract: request JSON in
// (argv[0]), /out/manifest.json plus the files it names out. A failure of one export is recorded in the
// manifest and the tool still exits 0; a request, provider or mount failure exits non-zero.
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using BCnEncoder.Decoder;
using BCnEncoder.Shared;
using CUE4Parse.Compression;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.UE4.AssetRegistry;
using CUE4Parse.UE4.Assets.Exports.Texture;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace TakaroUeExport;

internal static class Program
{
    private const string ToolName = "takaro-ue-export";
    private const string ToolVersion = "2";
    private const int MaxEdge = 4096;
    private static readonly Regex IdPattern = new("^[a-z0-9][a-z0-9-]{0,63}$", RegexOptions.Compiled);
    private static readonly JsonSerializerOptions Pretty = new() { WriteIndented = true };

    private static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("usage: TakaroUeExport <request.json>");
            return 2;
        }

        var request = JsonDocument.Parse(File.ReadAllText(args[0])).RootElement;
        if (request.GetProperty("schemaVersion").GetInt32() != 1 || request.GetProperty("decoder").GetString() != "unreal")
        {
            Console.Error.WriteLine("unsupported request: schemaVersion 1 and decoder 'unreal' are required");
            return 2;
        }

        var settings = request.GetProperty("settings");
        var root = request.GetProperty("root").GetString()!;
        var exports = request.GetProperty("exports").EnumerateArray().ToList();
        var seen = new HashSet<string>();
        foreach (var export in exports)
        {
            var id = export.GetProperty("id").GetString()!;
            if (!IdPattern.IsMatch(id) || !seen.Add(id))
            {
                Console.Error.WriteLine($"invalid or duplicate export id '{id}'");
                return 2;
            }
        }

        var provider = OpenProvider(settings, root);
        const string outDir = "/out";
        Directory.CreateDirectory(Path.Combine(outDir, "json"));
        Directory.CreateDirectory(Path.Combine(outDir, "png"));

        var results = new List<Dictionary<string, object?>>();
        foreach (var export in exports)
        {
            var id = export.GetProperty("id").GetString()!;
            try
            {
                results.Add(Run(provider, export, id, outDir));
            }
            catch (ExportFailure failure)
            {
                results.Add(new() { ["id"] = id, ["ok"] = false, ["error"] = failure.Message, ["errorKind"] = failure.Kind });
            }
            catch (Exception exception)
            {
                results.Add(new()
                {
                    ["id"] = id, ["ok"] = false, ["errorKind"] = "failed",
                    ["error"] = $"{exception.GetType().Name}: {exception.Message}",
                });
            }
        }

        var manifest = new Dictionary<string, object?>
        {
            ["schemaVersion"] = 1,
            ["tool"] = new Dictionary<string, object?>
            {
                ["name"] = ToolName,
                ["version"] = ToolVersion,
                ["cue4parse"] = typeof(DefaultFileProvider).Assembly.GetName().Version?.ToString(),
                ["bcnEncoder"] = typeof(BcDecoder).Assembly.GetName().Version?.ToString(),
                ["dotnet"] = Environment.Version.ToString(),
                ["engine"] = settings.GetProperty("engine").GetString(),
                ["mounted"] = provider.MountedVfs.Count,
                ["unloaded"] = provider.UnloadedVfs.Count,
            },
            ["results"] = results,
        };
        File.WriteAllText(Path.Combine(outDir, "manifest.json"), System.Text.Json.JsonSerializer.Serialize(manifest, Pretty) + "\n");
        return 0;
    }

    private static DefaultFileProvider OpenProvider(JsonElement settings, string root)
    {
        var engineName = settings.GetProperty("engine").GetString()!;
        if (!Enum.TryParse<EGame>(engineName, out var engine))
        {
            throw new InvalidOperationException($"unknown engine '{engineName}'");
        }

        OodleHelper.Initialize(settings.GetProperty("oodle").GetString()!);
        var provider = new DefaultFileProvider(root, SearchOption.AllDirectories, new VersionContainer(engine), StringComparer.OrdinalIgnoreCase);
        provider.Initialize();
        if (settings.TryGetProperty("aesKeyEnv", out var keyEnv) && keyEnv.ValueKind == JsonValueKind.String)
        {
            var key = Environment.GetEnvironmentVariable(keyEnv.GetString()!)
                ?? throw new InvalidOperationException($"environment variable {keyEnv.GetString()} is not set");
            provider.SubmitKey(new FGuid(), new FAesKey(key));
        }

        provider.Mount();
        provider.LoadVirtualPaths();
        Console.Error.WriteLine($"mounted {provider.MountedVfs.Count} containers, {provider.Files.Count} files, {provider.UnloadedVfs.Count} unloaded");
        return provider;
    }

    private static Dictionary<string, object?> Run(DefaultFileProvider provider, JsonElement export, string id, string outDir)
    {
        return export.GetProperty("kind").GetString() switch
        {
            "asset-registry" => AssetRegistry(provider, export, id, outDir),
            "json" => PackageJson(provider, export, id, outDir),
            "texture" => Texture(provider, export, id, outDir),
            "list-files" => ListFiles(provider, export, id, outDir),
            var other => throw new ExportFailure("failed", $"unknown export kind '{other}'"),
        };
    }

    private static Dictionary<string, object?> Ok(string id, string file, Dictionary<string, object?>? meta = null) =>
        new() { ["id"] = id, ["ok"] = true, ["file"] = file, ["meta"] = meta ?? new Dictionary<string, object?>() };

    private static Dictionary<string, object?> AssetRegistry(DefaultFileProvider provider, JsonElement export, string id, string outDir)
    {
        var classes = export.GetProperty("classes").EnumerateArray().Select(c => c.GetString()!).ToHashSet(StringComparer.Ordinal);
        var tagNames = export.TryGetProperty("tags", out var requestedTags)
            ? requestedTags.EnumerateArray().Select(t => t.GetString()!).ToList()
            : new List<string> { "RowStructure" };
        var assets = new SortedDictionary<string, Dictionary<string, object?>>(StringComparer.Ordinal);
        var registries = provider.Files.Keys.Where(k => k.EndsWith("assetregistry.bin", StringComparison.OrdinalIgnoreCase)).OrderBy(k => k, StringComparer.Ordinal).ToList();
        foreach (var path in registries)
        {
            if (!provider.TryCreateReader(path, out var reader))
            {
                continue;
            }

            var state = new FAssetRegistryState(reader);
            foreach (var asset in state.PreallocatedAssetDataBuffers)
            {
                var assetClass = asset.AssetClass.Text;
                if (!classes.Contains(assetClass))
                {
                    continue;
                }

                var tags = new Dictionary<string, object?>();
                foreach (var tagName in tagNames)
                {
                    if (asset.TagsAndValues.TryGetValue(new CUE4Parse.UE4.Objects.UObject.FName(tagName), out var tagValue))
                    {
                        tags[tagName] = tagValue;
                    }
                }

                assets[asset.ObjectPath] = new()
                {
                    ["packageName"] = asset.PackageName.Text,
                    ["assetName"] = asset.AssetName.Text,
                    ["assetClass"] = assetClass,
                    ["tags"] = tags,
                };
            }
        }

        var path2 = $"json/{id}.json";
        File.WriteAllText(Path.Combine(outDir, path2), System.Text.Json.JsonSerializer.Serialize(new { registries, assets = assets.Values }, Pretty) + "\n");
        return Ok(id, path2, new() { ["assets"] = assets.Count, ["registries"] = registries.Count });
    }

    private static Dictionary<string, object?> ListFiles(DefaultFileProvider provider, JsonElement export, string id, string outDir)
    {
        var prefix = export.GetProperty("prefix").GetString()!;
        var files = provider.Files
            .Where(f => f.Key.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => f.Key, StringComparer.Ordinal)
            .Select(f => new { path = f.Key, container = (f.Value as CUE4Parse.UE4.VirtualFileSystem.VfsEntry)?.Vfs.Name, size = f.Value.Size })
            .ToList();
        var path = $"json/{id}.json";
        File.WriteAllText(Path.Combine(outDir, path), System.Text.Json.JsonSerializer.Serialize(new { files, virtualPaths = provider.VirtualPaths.OrderBy(p => p.Key, StringComparer.Ordinal).ToDictionary(p => p.Key, p => p.Value) }, Pretty) + "\n");
        return Ok(id, path, new() { ["files"] = files.Count });
    }

    private static Dictionary<string, object?> PackageJson(DefaultFileProvider provider, JsonElement export, string id, string outDir)
    {
        var package = export.GetProperty("package").GetString()!;
        var loaded = LoadPackage(provider, package);
        var json = JsonConvert.SerializeObject(loaded.GetExports(), Formatting.Indented);
        var path = $"json/{id}.json";
        File.WriteAllText(Path.Combine(outDir, path), json + "\n", new UTF8Encoding(false));
        return Ok(id, path, new() { ["exports"] = JArray.Parse(json).Count });
    }

    private static Dictionary<string, object?> Texture(DefaultFileProvider provider, JsonElement export, string id, string outDir)
    {
        var package = export.GetProperty("package").GetString()!;
        var objectName = export.GetProperty("object").GetString()!;
        var loaded = LoadPackage(provider, package);
        var texture = loaded.GetExports().OfType<UTexture2D>().FirstOrDefault(t => t.Name == objectName)
            ?? throw new ExportFailure("not-found", $"{package}.{objectName}");
        var mip = texture.GetFirstMip() ?? throw new ExportFailure("failed", "texture has no mip with data");
        var data = mip.BulkData.Data ?? throw new ExportFailure("failed", "mip has no data");
        var width = mip.SizeX;
        var height = mip.SizeY;
        if (width <= 0 || height <= 0 || width > MaxEdge || height > MaxEdge)
        {
            throw new ExportFailure("failed", $"texture size {width}x{height} outside 1..{MaxEdge}");
        }

        var format = texture.Format;
        var rgba = Decode(format, data, width, height);
        var path = $"png/{id}.png";
        File.WriteAllBytes(Path.Combine(outDir, path), Png.Encode(rgba, width, height, format == EPixelFormat.PF_G8 || format == EPixelFormat.PF_BC4));
        return Ok(id, path, new()
        {
            ["width"] = width,
            ["height"] = height,
            ["format"] = format.ToString(),
            ["mip"] = Array.IndexOf(texture.PlatformData.Mips, mip),
            ["bulk"] = BulkKind(mip.BulkData.BulkDataFlags.ToString()),
        });
    }

    // A package that is not mounted is "not-found"; one that is mounted but cannot be read reports why.
    private static CUE4Parse.UE4.Assets.IPackage LoadPackage(DefaultFileProvider provider, string package)
    {
        if (provider.TryLoadPackage(package, out var loaded))
        {
            return loaded;
        }

        try
        {
            return provider.LoadPackage(package);
        }
        catch (Exception exception) when (exception is KeyNotFoundException or FileNotFoundException)
        {
            throw new ExportFailure("not-found", package);
        }
    }

    private static string BulkKind(string flags) =>
        flags.Contains("OptionalPayload") ? "optional" : flags.Contains("PayloadInSeperateFile") || flags.Contains("PayloadAtEndOfFile") ? "bulk" : "inline";

    private static byte[] Decode(EPixelFormat format, byte[] data, int width, int height)
    {
        var decoder = new BcDecoder();
        switch (format)
        {
            case EPixelFormat.PF_DXT1: return Flatten(decoder.DecodeRaw(data, width, height, CompressionFormat.Bc1WithAlpha));
            case EPixelFormat.PF_DXT5: return Flatten(decoder.DecodeRaw(data, width, height, CompressionFormat.Bc3));
            case EPixelFormat.PF_BC4: return Flatten(decoder.DecodeRaw(data, width, height, CompressionFormat.Bc4));
            case EPixelFormat.PF_BC5: return Flatten(decoder.DecodeRaw(data, width, height, CompressionFormat.Bc5));
            case EPixelFormat.PF_BC7: return Flatten(decoder.DecodeRaw(data, width, height, CompressionFormat.Bc7));
            case EPixelFormat.PF_B8G8R8A8:
                RequireLength(data, width * height * 4);
                var swapped = new byte[width * height * 4];
                for (var i = 0; i < swapped.Length; i += 4)
                {
                    swapped[i] = data[i + 2];
                    swapped[i + 1] = data[i + 1];
                    swapped[i + 2] = data[i];
                    swapped[i + 3] = data[i + 3];
                }

                return swapped;
            case EPixelFormat.PF_G8:
                RequireLength(data, width * height);
                var gray = new byte[width * height * 4];
                for (var i = 0; i < width * height; i++)
                {
                    gray[i * 4] = gray[i * 4 + 1] = gray[i * 4 + 2] = data[i];
                    gray[i * 4 + 3] = 255;
                }

                return gray;
            default:
                throw new ExportFailure("unsupported-format", format.ToString());
        }
    }

    private static void RequireLength(byte[] data, int expected)
    {
        if (data.Length < expected)
        {
            throw new ExportFailure("failed", $"mip holds {data.Length} bytes, {expected} expected");
        }
    }

    private static byte[] Flatten(BCnEncoder.Shared.ColorRgba32[] pixels)
    {
        var bytes = new byte[pixels.Length * 4];
        for (var i = 0; i < pixels.Length; i++)
        {
            bytes[i * 4] = pixels[i].r;
            bytes[i * 4 + 1] = pixels[i].g;
            bytes[i * 4 + 2] = pixels[i].b;
            bytes[i * 4 + 3] = pixels[i].a;
        }

        return bytes;
    }
}

internal sealed class ExportFailure(string kind, string message) : Exception(message)
{
    public string Kind { get; } = kind;
}
