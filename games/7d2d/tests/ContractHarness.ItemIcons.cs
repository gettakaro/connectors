using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Takaro;
using Takaro.Services;
using Takaro.WebSocket;

// items.icons: what the catalogue hashes, and the bounds of a getItemIcons answer.
public static partial class ContractHarness
{
    private static readonly byte[] PngHeader = { 137, 80, 78, 71, 13, 10, 26, 10 };

    private static string IconDirectory()
    {
        string directory = Path.Combine(Path.GetTempPath(), "takaro-icons-" + Guid.NewGuid());
        Directory.CreateDirectory(directory);
        return directory;
    }

    // A distinct payload per seed, so each file hashes differently.
    private static byte[] Png(int size, int seed)
    {
        var bytes = new byte[size];
        new Random(seed).NextBytes(bytes);
        PngHeader.CopyTo(bytes, 0);
        return bytes;
    }

    private static string WriteIcon(string directory, string name, byte[] bytes)
    {
        File.WriteAllBytes(Path.Combine(directory, name + ".png"), bytes);
        using (SHA256 sha256 = SHA256.Create())
            return string.Concat(sha256.ComputeHash(bytes).Select(b => b.ToString("x2")));
    }

    private static void FamilyItemIconCatalogue()
    {
        string vanilla = IconDirectory();
        string mod = IconDirectory();
        string shared = WriteIcon(vanilla, "sharedIcon", Png(64, 1));
        string replaced = WriteIcon(vanilla, "replacedIcon", Png(64, 2));
        string modded = WriteIcon(mod, "modIcon", Png(64, 3));
        string replacement = WriteIcon(mod, "replacedIcon", Png(64, 4));
        File.WriteAllBytes(Path.Combine(vanilla, "notAnImage.png"), new byte[64]);
        WriteIcon(vanilla, "tooLarge", Png(ItemIconCatalog.MaxResponseBytes + 1, 5));

        ItemIconCatalog catalog = ItemIconCatalog.Build(
            new[] { vanilla, mod, vanilla + "-absent" }
        );

        Equal(shared, catalog.HashFor("sharedIcon"), "iconHash is the SHA-256 of the PNG bytes");
        Equal(modded, catalog.HashFor("modIcon"), "a mod's icon is reported");
        Equal(replacement, catalog.HashFor("replacedIcon"), "a later folder wins an icon name");
        Equal(modded, catalog.HashFor("MODICON"), "icon names match without regard to case");
        True(catalog.HashFor("notAnImage") == null, "a file that is not a PNG reports no hash");
        True(catalog.HashFor("tooLarge") == null, "an icon over the bound reports no hash");
        True(catalog.HashFor("unknown") == null, "an item without art reports no hash");
        True(catalog.HashFor(null) == null, "an item without an icon name reports no hash");
        Equal(
            modded,
            ItemIconCatalog.Build(new[] { vanilla, mod }).HashFor("modIcon"),
            "a rebuild of unchanged files reports the same hash"
        );
        True(
            catalog.Serve(new[] { replaced }).Count == 0,
            "a replaced icon's old bytes are not served"
        );

        List<string> directories = ItemIconCatalog.IconDirectories("/game", new[] { "/mods/a" });
        True(
            directories.SequenceEqual(
                new[]
                {
                    Path.Combine("/game", "Data", "ItemIcons"),
                    Path.Combine("/mods/a", "ItemIcons"),
                    Path.Combine("/mods/a", "UIAtlases", "ItemIconAtlas"),
                }
            ),
            "icons come from the vanilla folder, then each mod's two folders"
        );
    }

    private static void FamilyItemIconServeBounds()
    {
        string directory = IconDirectory();
        const int size = ItemIconCatalog.MaxResponseBytes / 3;
        List<string> hashes = Enumerable
            .Range(0, 5)
            .Select(i => WriteIcon(directory, "big" + i, Png(size, 100 + i)))
            .ToList();
        ItemIconCatalog catalog = ItemIconCatalog.Build(new[] { directory });

        const string unknown = "0000000000000000000000000000000000000000000000000000000000000000";
        List<ItemIconEntry> first = catalog.Serve(
            new[] { hashes[0], unknown }.Concat(hashes.Skip(1))
        );
        True(
            first.Select(e => e.Hash).SequenceEqual(hashes.Take(3)),
            "an unknown hash is skipped and the walk stops before the bound, in request order"
        );
        True(
            first.Sum(e => Convert.FromBase64String(e.Data).Length)
                <= ItemIconCatalog.MaxResponseBytes,
            "a response stays within 1 MiB of decoded PNG"
        );
        True(
            Convert
                .FromBase64String(first[1].Data)
                .SequenceEqual(File.ReadAllBytes(Path.Combine(directory, "big1.png"))),
            "the exact file bytes are served"
        );
        True(
            catalog.Serve(hashes.Skip(3)).Select(e => e.Hash).SequenceEqual(hashes.Skip(3)),
            "the hashes the bound cut off are served when Takaro asks again"
        );
        True(catalog.Serve(new[] { unknown }).Count == 0, "an empty answer is legal");

        File.WriteAllBytes(Path.Combine(directory, "big0.png"), Png(64, 999));
        True(
            catalog.Serve(new[] { hashes[0] }).Count == 0,
            "a file changed since the build is skipped, never served under the old hash"
        );
    }

    private static JObject RouteGetItemIcons(object hashes)
    {
        WebSocketTransport.Instance.TerminalMessages.Clear();
        WebSocketTransport.Instance.ErrorCodes.Clear();
        RequestRouter.Route(
            new JObject
            {
                ["type"] = "request",
                ["requestId"] = "icons-1",
                ["payload"] = new JObject
                {
                    ["action"] = "getItemIcons",
                    ["args"] = JsonConvert.SerializeObject(new { hashes }),
                },
            }.ToString(Formatting.None)
        );
        return SerializeFrame(WebSocketTransport.Instance.TerminalMessages.Single());
    }

    private static void FamilyItemIconRouting()
    {
        string directory = IconDirectory();
        string hash = WriteIcon(directory, "icon", Png(64, 7));
        ItemIconCatalog previous = ItemIconCatalog.Current;
        ItemIconCatalog.Current = ItemIconCatalog.Build(new[] { directory });
        WebSocketTransport.Instance.ProtocolVersion = 1;
        try
        {
            JObject answer = RouteGetItemIcons(new[] { hash });
            Equal("response", (string)answer["type"], "getItemIcons answers with a response");
            JArray entries = (JArray)answer["payload"];
            Equal(hash, (string)entries.Single()["hash"], "the entry names the requested hash");
            True(
                ((JObject)entries[0])
                    .Properties()
                    .Select(p => p.Name)
                    .OrderBy(n => n, StringComparer.Ordinal)
                    .SequenceEqual(new[] { "data", "hash" }),
                "an entry carries exactly hash and data"
            );

            RouteGetItemIcons(Enumerable.Repeat(hash, ItemIconCatalog.MaxBatchHashes + 1));
            Equal(
                ProtocolErrorCodes.InvalidArgs,
                WebSocketTransport.Instance.ErrorCodes.Single(),
                "more than 100 hashes is refused"
            );
            foreach (
                object invalid in new object[]
                {
                    new string[0],
                    new[] { "ABC" },
                    new[] { hash.ToUpperInvariant() },
                }
            )
            {
                RouteGetItemIcons(invalid);
                Equal(
                    ProtocolErrorCodes.InvalidArgs,
                    WebSocketTransport.Instance.ErrorCodes.Single(),
                    "a malformed hash list is refused"
                );
            }
        }
        finally
        {
            ItemIconCatalog.Current = previous;
        }
    }

    private static void FamilyItemIconManifest()
    {
        Func<bool, IEnumerable<string>> capabilities = hasIcons =>
            ProtocolHandshake
                .BuildCapabilityManifest(ProtocolHandshake.DeclaredActions(hasIcons))
                .Select(c => (string)c["name"]);
        True(capabilities(true).Contains("items.icons"), "a server with art declares items.icons");
        True(!capabilities(false).Contains("items.icons"), "a server without art does not");

        JObject item = JObject.FromObject(new TakaroItem { Code = "a", Name = "A" });
        True(item["iconHash"] == null, "an item without art has no iconHash on the wire");
        item = JObject.FromObject(
            new TakaroItem
            {
                Code = "a",
                Name = "A",
                IconHash = "h",
            }
        );
        Equal("h", (string)item["iconHash"], "an item with art reports iconHash");
    }
}
