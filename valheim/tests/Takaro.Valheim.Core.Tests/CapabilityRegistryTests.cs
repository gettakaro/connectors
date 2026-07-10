using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CapabilityRegistryTests
{
    private static readonly string[] RequiredActions =
    [
        "testReachability",
        "getPlayers",
        "getPlayer",
        "getPlayerLocation",
        "getPlayerInventory",
        "giveItem",
        "sendMessage",
        "executeConsoleCommand",
        "listItems",
        "listEntities",
        "listLocations",
        "getMapInfo",
        "getMapTile",
        "teleportPlayer",
        "kickPlayer",
        "banPlayer",
        "unbanPlayer",
        "listBans",
        "shutdown"
    ];

    private static readonly string[] RequiredEvents =
    [
        "log",
        "player-connected",
        "player-disconnected",
        "chat-message",
        "player-death",
        "entity-killed"
    ];

    private static readonly HashSet<string> AllowedStatuses =
    [
        "live-supported",
        "schema-fallback",
        "unsupported"
    ];

    [TestMethod]
    public void RegistryExhaustivelyClassifiesEveryActionAndEvent()
    {
        using var registry = ReadRegistry();
        var root = registry.RootElement;

        Assert.AreEqual(JsonValueKind.String, root.GetProperty("architecture").ValueKind);
        Assert.AreEqual(JsonValueKind.Array, root.GetProperty("notes").ValueKind);
        AssertCompleteRegistry(root.GetProperty("actions"), RequiredActions, "action");
        AssertCompleteRegistry(root.GetProperty("events"), RequiredEvents, "event");
    }

    [TestMethod]
    public void PinnedTakaroActionListAt0c63cf1cMatchesDispatcherConstantsAndRegistry()
    {
        var upstreamActionsAt0c63cf1c = new[]
        {
            "testReachability", "getPlayers", "getPlayer", "getPlayerLocation",
            "getPlayerInventory", "giveItem", "sendMessage", "executeConsoleCommand",
            "listItems", "listEntities", "listLocations", "getMapInfo", "getMapTile",
            "teleportPlayer", "kickPlayer", "banPlayer", "unbanPlayer", "listBans", "shutdown"
        };

        CollectionAssert.AreEqual(upstreamActionsAt0c63cf1c, TakaroActionNames.All.ToArray());

        using var registry = ReadRegistry();
        CollectionAssert.AreEquivalent(
            upstreamActionsAt0c63cf1c,
            registry.RootElement.GetProperty("actions").EnumerateObject().Select(entry => entry.Name).ToArray());
    }

    [TestMethod]
    public void RegistryKeepsUnprovenClientOwnedAndServerEventPathsUnsupported()
    {
        using var registry = ReadRegistry();
        var root = registry.RootElement;

        Assert.AreEqual("unsupported", root.GetProperty("actions").GetProperty("getPlayerInventory").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("actions").GetProperty("getMapInfo").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("actions").GetProperty("getMapTile").GetString());
        Assert.AreEqual("schema-fallback", root.GetProperty("actions").GetProperty("listLocations").GetString());
        Assert.AreEqual("live-supported", root.GetProperty("events").GetProperty("player-connected").GetString());
        Assert.AreEqual("live-supported", root.GetProperty("events").GetProperty("player-disconnected").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("events").GetProperty("chat-message").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("events").GetProperty("player-death").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("events").GetProperty("entity-killed").GetString());
    }

    [TestMethod]
    public void ListLocationsSeparatesRawConnectorProofFromUnavailableStandardTakaroRoute()
    {
        using var document = ReadRegistry();
        var notes = document.RootElement.GetProperty("notes")
            .EnumerateArray()
            .Select(note => note.GetString() ?? string.Empty)
            .ToArray();
        var readme = ReadValheimFile("README.md");

        Assert.IsTrue(notes.Any(note => note.Contains("raw Generic Connector action", StringComparison.Ordinal)));
        Assert.IsTrue(notes.Any(note => note.Contains("standard Takaro route", StringComparison.Ordinal)));
        StringAssert.Contains(readme, "| `listLocations` | `schema-fallback`");
        StringAssert.Contains(readme, "official raw Generic Connector action/schema");
        StringAssert.Contains(readme, "standard Takaro route");
    }

    [TestMethod]
    public void ReadmeDoesNotAdvertiseMissingValheimJustRecipes()
    {
        var readme = ReadValheimFile("README.md");
        var justfile = ReadValheimFile("../justfile");

        Assert.IsFalse(readme.Contains("just valheim-setup", StringComparison.Ordinal));
        Assert.IsFalse(readme.Contains("just build-release-valheim", StringComparison.Ordinal));
        StringAssert.Contains(readme, "./scripts/setup-environment.sh");
        Assert.IsFalse(justfile.Contains("valheim-setup:", StringComparison.Ordinal));
        Assert.IsFalse(justfile.Contains("build-release-valheim ", StringComparison.Ordinal));
    }

    [TestMethod]
    public void RegistryAndReadmeSupportMatricesStaySynchronized()
    {
        using var registry = ReadRegistry();
        var readme = ReadValheimFile("README.md");

        foreach (var sectionName in new[] { "actions", "events" })
        {
            foreach (var entry in registry.RootElement.GetProperty(sectionName).EnumerateObject())
            {
                StringAssert.Contains(
                    readme,
                    $"| `{entry.Name}` | `{entry.Value.GetString()}` |",
                    $"README status mismatch for {entry.Name}.");
            }
        }
    }

    private static JsonDocument ReadRegistry()
    {
        return JsonDocument.Parse(ReadValheimFile("capabilities.json"));
    }

    private static string ReadValheimFile(string relativePath)
    {
        var path = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../",
            relativePath));

        return File.ReadAllText(path);
    }

    private static void AssertCompleteRegistry(JsonElement entries, string[] expectedNames, string kind)
    {
        Assert.AreEqual(JsonValueKind.Object, entries.ValueKind, kind);
        var actualNames = entries.EnumerateObject().Select(entry => entry.Name).Order().ToArray();
        var expected = expectedNames.Order().ToArray();
        CollectionAssert.AreEqual(expected, actualNames, $"Unexpected {kind} registry keys.");

        foreach (var entry in entries.EnumerateObject())
        {
            Assert.AreEqual(JsonValueKind.String, entry.Value.ValueKind, entry.Name);
            Assert.IsTrue(AllowedStatuses.Contains(entry.Value.GetString()!), $"Invalid status for {entry.Name}.");
        }
    }
}
