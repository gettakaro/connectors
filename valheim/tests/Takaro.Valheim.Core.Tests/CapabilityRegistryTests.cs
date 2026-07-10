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
    public void RegistryKeepsUnprovenClientOwnedAndServerEventPathsUnsupported()
    {
        using var registry = ReadRegistry();
        var root = registry.RootElement;

        Assert.AreEqual("unsupported", root.GetProperty("actions").GetProperty("getPlayerInventory").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("events").GetProperty("chat-message").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("events").GetProperty("player-death").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("events").GetProperty("entity-killed").GetString());
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
