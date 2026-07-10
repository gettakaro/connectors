using System.Text.Json;
using System.Text.Json.Serialization;

namespace Takaro.Valheim.Core;

public interface IValheimTakaroAdapter
{
    Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetPlayersAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GiveItemAsync(string identifier, string itemCode, int amount, string? quality, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> SendMessageAsync(string message, string? recipientIdentifier, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ListEntitiesAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetMapTileAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default);
}

public static class TakaroActionNames
{
    public const string TestReachability = "testReachability";
    public const string GetPlayers = "getPlayers";
    public const string GetPlayer = "getPlayer";
    public const string GetPlayerLocation = "getPlayerLocation";
    public const string GetPlayerInventory = "getPlayerInventory";
    public const string GiveItem = "giveItem";
    public const string SendMessage = "sendMessage";
    public const string ExecuteConsoleCommand = "executeConsoleCommand";
    public const string ListItems = "listItems";
    public const string ListEntities = "listEntities";
    public const string ListLocations = "listLocations";
    public const string GetMapInfo = "getMapInfo";
    public const string GetMapTile = "getMapTile";
    public const string TeleportPlayer = "teleportPlayer";
    public const string KickPlayer = "kickPlayer";
    public const string BanPlayer = "banPlayer";
    public const string UnbanPlayer = "unbanPlayer";
    public const string ListBans = "listBans";
    public const string Shutdown = "shutdown";

    public static IReadOnlyList<string> All { get; } =
    [
        TestReachability,
        GetPlayers,
        GetPlayer,
        GetPlayerLocation,
        GetPlayerInventory,
        GiveItem,
        SendMessage,
        ExecuteConsoleCommand,
        ListItems,
        ListEntities,
        ListLocations,
        GetMapInfo,
        GetMapTile,
        TeleportPlayer,
        KickPlayer,
        BanPlayer,
        UnbanPlayer,
        ListBans,
        Shutdown
    ];
}

public sealed record TakaroActionResult(
    [property: JsonPropertyName("success")] bool Success,
    [property: JsonPropertyName("payload")] object? Payload,
    [property: JsonPropertyName("errorCode")] string? ErrorCode,
    [property: JsonPropertyName("message")] string? Message)
{
    public static TakaroActionResult Ok(object? payload = null) => new(true, payload, null, null);

    public static TakaroActionResult Error(string errorCode, string message, object? payload = null) =>
        new(false, payload, errorCode, message);
}

public sealed record TakaroPosition(double X, double Y, double Z, string? Dimension = null);

public sealed class TakaroRequestDispatcher
{
    private readonly IValheimTakaroAdapter adapter;

    public TakaroRequestDispatcher(IValheimTakaroAdapter adapter)
    {
        this.adapter = adapter;
    }

    public async Task<TakaroActionResult> DispatchAsync(TakaroRequest request, CancellationToken cancellationToken = default)
    {
        try
        {
            return request.Action switch
            {
                TakaroActionNames.TestReachability => await adapter.TestReachabilityAsync(cancellationToken),
                TakaroActionNames.GetPlayers => await adapter.GetPlayersAsync(cancellationToken),
                TakaroActionNames.GetPlayer => await adapter.GetPlayerAsync(RequiredIdentifier(request.Args), cancellationToken),
                TakaroActionNames.GetPlayerLocation => await adapter.GetPlayerLocationAsync(RequiredIdentifier(request.Args), cancellationToken),
                TakaroActionNames.GetPlayerInventory => await adapter.GetPlayerInventoryAsync(RequiredIdentifier(request.Args), cancellationToken),
                TakaroActionNames.GiveItem => await adapter.GiveItemAsync(RequiredIdentifier(request.Args), RequiredItemCode(request.Args), OptionalPositiveInt(request.Args, "amount") ?? OptionalPositiveInt(request.Args, "quantity") ?? 1, OptionalString(request.Args, "quality"), cancellationToken),
                TakaroActionNames.SendMessage => await adapter.SendMessageAsync(RequiredString(request.Args, "message"), OptionalRecipientIdentifier(request.Args), cancellationToken),
                TakaroActionNames.ExecuteConsoleCommand => await adapter.ExecuteConsoleCommandAsync(RequiredString(request.Args, "command"), cancellationToken),
                TakaroActionNames.ListItems => await adapter.ListItemsAsync(cancellationToken),
                TakaroActionNames.ListEntities => await adapter.ListEntitiesAsync(cancellationToken),
                TakaroActionNames.ListLocations => await adapter.ListLocationsAsync(cancellationToken),
                TakaroActionNames.GetMapInfo => await adapter.GetMapInfoAsync(cancellationToken),
                TakaroActionNames.GetMapTile => await adapter.GetMapTileAsync(cancellationToken),
                TakaroActionNames.TeleportPlayer => await adapter.TeleportPlayerAsync(RequiredIdentifier(request.Args), RequiredPosition(request.Args), cancellationToken),
                TakaroActionNames.KickPlayer => NullPayloadOnSuccess(await adapter.KickPlayerAsync(RequiredIdentifier(request.Args), OptionalString(request.Args, "reason"), cancellationToken)),
                TakaroActionNames.BanPlayer => NullPayloadOnSuccess(await adapter.BanPlayerAsync(RequiredIdentifier(request.Args), OptionalString(request.Args, "reason"), cancellationToken)),
                TakaroActionNames.UnbanPlayer => NullPayloadOnSuccess(await adapter.UnbanPlayerAsync(RequiredIdentifier(request.Args), cancellationToken)),
                TakaroActionNames.ListBans => await adapter.ListBansAsync(cancellationToken),
                TakaroActionNames.Shutdown => NullPayloadOnSuccess(await adapter.ShutdownAsync(cancellationToken)),
                _ => TakaroActionResult.Error("unsupported_action", $"Valheim connector does not support action '{request.Action}' yet.")
            };
        }
        catch (ArgumentException ex)
        {
            return TakaroActionResult.Error("invalid_args", ex.Message);
        }
        catch (Exception ex)
        {
            return TakaroActionResult.Error("action_failed", ex.Message);
        }
    }

    private static string RequiredIdentifier(JsonElement args) =>
        OptionalIdentifier(args)
        ?? throw new ArgumentException("Expected one of gameId, platformId, steamId, or name.");

    private static TakaroActionResult NullPayloadOnSuccess(TakaroActionResult result) =>
        result.Success ? result with { Payload = null } : result;

    private static string? OptionalIdentifier(JsonElement args) =>
        OptionalString(args, "gameId")
        ?? OptionalString(args, "platformId")
        ?? OptionalString(args, "steamId")
        ?? OptionalString(args, "name")
        ?? OptionalNestedIdentifier(args, "player");

    private static string? OptionalRecipientIdentifier(JsonElement args)
    {
        if (args.ValueKind == JsonValueKind.Object
            && args.TryGetProperty("opts", out var opts)
            && opts.ValueKind == JsonValueKind.Object
            && opts.TryGetProperty("recipient", out var recipient))
        {
            return OptionalIdentifier(recipient);
        }

        return OptionalNestedIdentifier(args, "recipient");
    }

    private static string? OptionalNestedIdentifier(JsonElement args, string property)
    {
        if (args.ValueKind != JsonValueKind.Object || !args.TryGetProperty(property, out var nested))
        {
            return null;
        }

        return OptionalIdentifier(nested);
    }

    private static string RequiredString(JsonElement args, string property) =>
        OptionalString(args, property) ?? throw new ArgumentException($"Expected string argument '{property}'.");

    private static string RequiredItemCode(JsonElement args) =>
        OptionalString(args, "item")
        ?? OptionalString(args, "itemId")
        ?? OptionalString(args, "code")
        ?? OptionalNestedString(args, "item", "code")
        ?? OptionalNestedString(args, "item", "id")
        ?? OptionalNestedString(args, "item", "name")
        ?? throw new ArgumentException("Expected one of item, itemId, code, or item.code.");

    private static TakaroPosition RequiredPosition(JsonElement args)
    {
        var source = args;
        if (args.ValueKind == JsonValueKind.Object)
        {
            if (args.TryGetProperty("position", out var position) && position.ValueKind == JsonValueKind.Object)
            {
                source = position;
            }
            else if (args.TryGetProperty("location", out var location) && location.ValueKind == JsonValueKind.Object)
            {
                source = location;
            }
        }

        return new TakaroPosition(
            RequiredNumber(source, "x"),
            RequiredNumber(source, "y"),
            RequiredNumber(source, "z"),
            OptionalString(source, "dimension"));
    }

    private static string? OptionalString(JsonElement args, string property)
    {
        if (args.ValueKind != JsonValueKind.Object
            || !args.TryGetProperty(property, out var value)
            || value.ValueKind != JsonValueKind.String)
        {
            return null;
        }

        var text = value.GetString()?.Trim();
        return string.IsNullOrEmpty(text) ? null : text;
    }

    private static string? OptionalNestedString(JsonElement args, string property, string nestedProperty)
    {
        if (args.ValueKind != JsonValueKind.Object
            || !args.TryGetProperty(property, out var nested)
            || nested.ValueKind != JsonValueKind.Object)
        {
            return null;
        }

        return OptionalString(nested, nestedProperty);
    }

    private static int? OptionalPositiveInt(JsonElement args, string property)
    {
        if (args.ValueKind != JsonValueKind.Object
            || !args.TryGetProperty(property, out var value))
        {
            return null;
        }

        if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out var number))
        {
            throw new ArgumentException($"Expected integer argument '{property}'.");
        }

        if (number <= 0)
        {
            throw new ArgumentException($"Expected positive integer argument '{property}'.");
        }

        if (number > GiveItemPolicy.MaxAmount)
        {
            throw new ArgumentException(
                $"Expected integer argument '{property}' to be at most {GiveItemPolicy.MaxAmount}.");
        }

        return number;
    }

    private static double RequiredNumber(JsonElement args, string property)
    {
        if (args.ValueKind != JsonValueKind.Object
            || !args.TryGetProperty(property, out var value)
            || value.ValueKind != JsonValueKind.Number
            || !value.TryGetDouble(out var number))
        {
            throw new ArgumentException($"Expected numeric argument '{property}'.");
        }

        return number;
    }
}
