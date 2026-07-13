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

    private static readonly HashSet<string> AllowedOwnership =
    [
        "server-owned",
        "client-reported",
        "upstream-blocked",
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
        var ownership = root.GetProperty("ownership");
        AssertCompleteOwnership(
            ownership.GetProperty("actions"),
            RequiredActions,
            "action");
        AssertCompleteOwnership(
            ownership.GetProperty("events"),
            RequiredEvents,
            "event");
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
    public void RegistryPublishesLiveProvenClientOwnedPaths()
    {
        using var registry = ReadRegistry();
        var root = registry.RootElement;

        Assert.AreEqual("live-supported", root.GetProperty("actions").GetProperty("getPlayerInventory").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("actions").GetProperty("getMapInfo").GetString());
        Assert.AreEqual("unsupported", root.GetProperty("actions").GetProperty("getMapTile").GetString());
        Assert.AreEqual("schema-fallback", root.GetProperty("actions").GetProperty("listLocations").GetString());
        Assert.AreEqual("live-supported", root.GetProperty("events").GetProperty("player-connected").GetString());
        Assert.AreEqual("live-supported", root.GetProperty("events").GetProperty("player-disconnected").GetString());
        Assert.AreEqual("live-supported", root.GetProperty("events").GetProperty("chat-message").GetString());
        Assert.AreEqual("live-supported", root.GetProperty("events").GetProperty("player-death").GetString());
        Assert.AreEqual("live-supported", root.GetProperty("events").GetProperty("entity-killed").GetString());

        var ownership = root.GetProperty("ownership");
        Assert.AreEqual(
            "client-reported",
            ownership.GetProperty("actions").GetProperty("getPlayerInventory").GetString());
        Assert.AreEqual(
            "upstream-blocked",
            ownership.GetProperty("actions").GetProperty("listLocations").GetString());
        Assert.AreEqual(
            "unsupported",
            ownership.GetProperty("actions").GetProperty("getMapInfo").GetString());
        foreach (var eventName in new[] { "chat-message", "player-death", "entity-killed" })
        {
            Assert.AreEqual(
                "client-reported",
                ownership.GetProperty("events").GetProperty(eventName).GetString(),
                eventName);
        }
    }

    [TestMethod]
    public void ApprovalGatedDestructiveActionsRemainUnsupportedUntilExactLiveProofExists()
    {
        using var registry = ReadRegistry();
        var root = registry.RootElement;
        var notes = root.GetProperty("notes")
            .EnumerateArray()
            .Select(note => note.GetString() ?? string.Empty)
            .ToArray();
        var readme = ReadValheimFile("README.md");

        foreach (var action in new[] { "kickPlayer", "banPlayer", "unbanPlayer", "shutdown" })
        {
            Assert.AreEqual("unsupported", root.GetProperty("actions").GetProperty(action).GetString(), action);
            StringAssert.Contains(readme, $"| `{action}` | `unsupported` |");
        }

        Assert.IsTrue(notes.Any(note =>
            note.Contains("implementation exists", StringComparison.OrdinalIgnoreCase)
            && note.Contains("approval-gated", StringComparison.OrdinalIgnoreCase)
            && note.Contains("exact live support is unproven", StringComparison.OrdinalIgnoreCase)));
        StringAssert.Contains(readme, "exact live support remains unproven and approval-gated");
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

    [TestMethod]
    public void CompanionDocumentationCoversSafeInstallUpgradeRemovalAndTrustBoundary()
    {
        var companion = ReadValheimFile("COMPANION.md");
        var readme = ReadValheimFile("README.md");
        var combined = companion + "\n" + readme;

        foreach (var marker in new[]
                 {
                     "takaro-valheim-plugin.zip",
                     "takaro-valheim-companion.zip",
                     "Install",
                     "Upgrade",
                     "Remove",
                     "required",
                     "protocol",
                     "client-reported",
                     "untrusted",
                     "No Takaro token",
                     "BepInEx/plugins/TakaroValheimCompanion"
                 })
        {
            StringAssert.Contains(combined, marker);
        }

        StringAssert.Contains(companion, "registrationToken stays on the dedicated server");
        StringAssert.Contains(companion, "expected and actual protocol versions");
        StringAssert.Contains(readme, "server plugin still refuses graphical-client processes");
    }

    [TestMethod]
    public void ServerMessageDocumentationRequiresAuthenticatedNormalChatRendering()
    {
        var companion = ReadValheimFile("COMPANION.md");
        var readme = ReadValheimFile("README.md");
        using var registry = ReadRegistry();
        var notes = registry.RootElement.GetProperty("notes")
            .EnumerateArray()
            .Select(note => note.GetString() ?? string.Empty)
            .ToArray();

        StringAssert.Contains(readme, "normal Valheim chat history");
        StringAssert.Contains(readme, "active negotiated companion");
        StringAssert.Contains(companion, "server-chat");
        StringAssert.Contains(companion, "normal chat history");
        StringAssert.Contains(companion, "never rendered through the HUD overlay APIs");
        Assert.IsTrue(notes.Any(note =>
            note.Contains("sendMessage", StringComparison.Ordinal)
            && note.Contains("normal chat", StringComparison.OrdinalIgnoreCase)
            && note.Contains("negotiated companion", StringComparison.OrdinalIgnoreCase)));
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

    private static void AssertCompleteOwnership(
        JsonElement entries,
        string[] expectedNames,
        string kind)
    {
        Assert.AreEqual(JsonValueKind.Object, entries.ValueKind, kind);
        CollectionAssert.AreEqual(
            expectedNames.Order().ToArray(),
            entries.EnumerateObject().Select(entry => entry.Name).Order().ToArray(),
            $"Unexpected {kind} ownership keys.");
        foreach (var entry in entries.EnumerateObject())
        {
            Assert.IsTrue(
                AllowedOwnership.Contains(entry.Value.GetString()!),
                $"Invalid ownership for {entry.Name}.");
        }
    }
}
