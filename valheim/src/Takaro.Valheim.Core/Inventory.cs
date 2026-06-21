using System.Text.Json.Serialization;

namespace Takaro.Valheim.Core;

public sealed record TakaroInventoryItem(
    [property: JsonPropertyName("code")] string Code,
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("amount")] int Amount,
    [property: JsonPropertyName("quality")] string Quality,
    [property: JsonPropertyName("durability")] float? Durability = null,
    [property: JsonPropertyName("equipped")] bool? Equipped = null,
    [property: JsonPropertyName("position")] TakaroInventorySlot? Position = null);

public sealed record TakaroInventorySlot(
    [property: JsonPropertyName("x")] int X,
    [property: JsonPropertyName("y")] int Y);

public sealed record TakaroLocation(
    [property: JsonPropertyName("code")] string Code,
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("x")] double X,
    [property: JsonPropertyName("y")] double Y,
    [property: JsonPropertyName("z")] double Z,
    [property: JsonPropertyName("dimension")] string Dimension = "valheim");

public sealed record TakaroEntity(
    [property: JsonPropertyName("code")] string Code,
    [property: JsonPropertyName("name")] string Name);

public static class LocationFactory
{
    public static TakaroLocation Create(string code, string? rawName, double x, double y, double z) =>
        new(code, DisplayName(rawName, code), x, y, z);

    private static string DisplayName(string? rawName, string fallback)
    {
        if (string.IsNullOrWhiteSpace(rawName))
        {
            return fallback;
        }

        var displayName = rawName!.Trim().Trim('$');
        return string.IsNullOrWhiteSpace(displayName) ? fallback : displayName;
    }
}

public sealed class LocationSnapshotCache
{
    private readonly Dictionary<string, LocationSnapshot> snapshotsByAlias = new(StringComparer.OrdinalIgnoreCase);

    public bool Store(TakaroPlayer player, TakaroPosition position, DateTimeOffset timestamp)
    {
        var aliases = Aliases(player).ToArray();
        if (aliases.Any(alias =>
                snapshotsByAlias.TryGetValue(alias, out var existing) && timestamp < existing.Timestamp))
        {
            return false;
        }

        var snapshot = new LocationSnapshot(position, timestamp);
        foreach (var alias in aliases)
        {
            snapshotsByAlias[alias] = snapshot;
        }

        return true;
    }

    public bool TryGet(string identifier, out TakaroPosition position)
    {
        if (snapshotsByAlias.TryGetValue(identifier, out var snapshot))
        {
            position = snapshot.Position;
            return true;
        }

        position = new TakaroPosition(0, 0, 0, "valheim");
        return false;
    }

    private static IEnumerable<string> Aliases(TakaroPlayer player) =>
        new[] { player.GameId, player.Name, player.SteamId, player.PlatformId }
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .Select(value => value!);

    private sealed record LocationSnapshot(
        TakaroPosition Position,
        DateTimeOffset Timestamp);
}

public sealed class ConsoleCommandPolicy
{
    public static ConsoleCommandPolicy Default { get; } = new(new[] { "help" }, Array.Empty<string>());

    private readonly string[] exactCommands;
    private readonly string[] commandPrefixes;

    public ConsoleCommandPolicy(IEnumerable<string> exactCommands, IEnumerable<string> commandPrefixes)
    {
        this.exactCommands = exactCommands
            .Select(Normalize)
            .Where(command => !string.IsNullOrWhiteSpace(command))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
        this.commandPrefixes = commandPrefixes
            .Select(value => value?.TrimStart() ?? string.Empty)
            .Where(prefix => !string.IsNullOrWhiteSpace(prefix))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    public bool IsAllowed(string command)
    {
        var normalized = Normalize(command);
        if (string.IsNullOrWhiteSpace(normalized))
        {
            return false;
        }

        return exactCommands.Any(allowed => normalized.Equals(allowed, StringComparison.OrdinalIgnoreCase))
            || commandPrefixes.Any(prefix => PrefixMatches(normalized, prefix));
    }

    private static string Normalize(string command) =>
        command.Trim();

    private static bool PrefixMatches(string command, string configuredPrefix)
    {
        var prefix = configuredPrefix.Trim();
        if (prefix.Length == 0)
        {
            return false;
        }

        if (configuredPrefix.Length > 0
            && char.IsWhiteSpace(configuredPrefix[configuredPrefix.Length - 1]))
        {
            return command.StartsWith(configuredPrefix.TrimStart(), StringComparison.OrdinalIgnoreCase);
        }

        return command.Equals(prefix, StringComparison.OrdinalIgnoreCase)
            || command.StartsWith(prefix + " ", StringComparison.OrdinalIgnoreCase);
    }
}

public sealed class InventorySnapshotCache
{
    private readonly Dictionary<string, InventorySnapshot> snapshotsByAlias = new(StringComparer.OrdinalIgnoreCase);

    public bool Store(TakaroPlayer player, IReadOnlyList<TakaroInventoryItem> items, DateTimeOffset timestamp)
    {
        var aliases = Aliases(player).ToArray();
        if (aliases.Any(alias =>
                snapshotsByAlias.TryGetValue(alias, out var existing) && timestamp < existing.Timestamp))
        {
            return false;
        }

        if (items.Count == 0 && aliases.Any(alias =>
                snapshotsByAlias.TryGetValue(alias, out var existing) && existing.Items.Count > 0))
        {
            return false;
        }

        var snapshot = new InventorySnapshot(items.ToArray(), timestamp);
        foreach (var alias in aliases)
        {
            snapshotsByAlias[alias] = snapshot;
        }

        return true;
    }

    public bool TryGet(string identifier, out IReadOnlyList<TakaroInventoryItem> items)
    {
        if (snapshotsByAlias.TryGetValue(identifier, out var snapshot))
        {
            items = snapshot.Items;
            return true;
        }

        items = Array.Empty<TakaroInventoryItem>();
        return false;
    }

    private static IEnumerable<string> Aliases(TakaroPlayer player) =>
        new[] { player.GameId, player.Name, player.SteamId, player.PlatformId }
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .Select(value => value!);

    private sealed record InventorySnapshot(
        IReadOnlyList<TakaroInventoryItem> Items,
        DateTimeOffset Timestamp);
}
