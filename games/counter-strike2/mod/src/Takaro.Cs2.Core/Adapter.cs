using System.Text.Json;
using System.Text.Json.Serialization;

namespace Takaro.Cs2.Core;

public interface ICs2TakaroAdapter
{
    Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default);
    Task<IReadOnlyList<TakaroPlayer>> GetPlayersAsync(CancellationToken cancellationToken = default);
    Task<TakaroPlayer?> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> SendMessageAsync(string message, string? recipient, string? sender, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GiveItemAsync(string identifier, string code, int amount, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> GetMapTileAsync(int x, int y, int zoom, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, string? expiresAt, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default);
    Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default);
}

public sealed record TakaroPosition(double X, double Y, double Z);

public sealed record TakaroActionError(
    [property: JsonPropertyName("code")] string Code,
    [property: JsonPropertyName("message")] string Message);

public sealed record TakaroActionResult(
    [property: JsonPropertyName("success")] bool Success,
    [property: JsonPropertyName("payload")] object? Payload,
    [property: JsonPropertyName("error")] TakaroActionError? Error)
{
    public static TakaroActionResult Ok(object? payload = null) => new(true, payload, null);

    public static TakaroActionResult Fail(string code, string message, object? payload = null) =>
        new(false, payload, new TakaroActionError(code, message));
}

public sealed class TakaroRequestDispatcher
{
    private readonly ICs2TakaroAdapter adapter;

    public TakaroRequestDispatcher(ICs2TakaroAdapter adapter)
    {
        this.adapter = adapter;
    }

    public async Task<TakaroActionResult> DispatchAsync(TakaroRequest request, CancellationToken cancellationToken = default)
    {
        try
        {
            return request.Action switch
            {
                "testReachability" => await adapter.TestReachabilityAsync(cancellationToken),
                "getPlayers" => TakaroActionResult.Ok(await adapter.GetPlayersAsync(cancellationToken)),
                "getPlayer" => TakaroActionResult.Ok(await adapter.GetPlayerAsync(RequiredIdentifier(request.Args), cancellationToken)),
                "getPlayerLocation" => await adapter.GetPlayerLocationAsync(RequiredIdentifier(request.Args), cancellationToken),
                "getPlayerInventory" => await adapter.GetPlayerInventoryAsync(RequiredIdentifier(request.Args), cancellationToken),
                "giveItem" => await adapter.GiveItemAsync(RequiredIdentifier(request.Args), RequiredItemCode(request.Args), OptionalPositiveInt(request.Args, "amount") ?? OptionalPositiveInt(request.Args, "quantity") ?? 1, cancellationToken),
                "listItems" => await adapter.ListItemsAsync(cancellationToken),
                "listEntities" => TakaroActionResult.Ok(new[] { new { code = "chicken", name = "Chicken", type = "neutral" } }),
                "listLocations" => await adapter.ListLocationsAsync(cancellationToken),
                "getMapInfo" => await adapter.GetMapInfoAsync(cancellationToken),
                "getMapTile" => await adapter.GetMapTileAsync(RequiredMapInt(request.Args, "x"), RequiredMapInt(request.Args, "y"), RequiredMapZoom(request.Args), cancellationToken),
                "sendMessage" => await adapter.SendMessageAsync(RequiredString(request.Args, "message"), OptionalRecipient(request.Args), OptionalSender(request.Args), cancellationToken),
                "executeConsoleCommand" => await adapter.ExecuteConsoleCommandAsync(RequiredString(request.Args, "command"), cancellationToken),
                "teleportPlayer" => await adapter.TeleportPlayerAsync(RequiredIdentifier(request.Args), RequiredPosition(request.Args), cancellationToken),
                "kickPlayer" => await adapter.KickPlayerAsync(RequiredIdentifier(request.Args), OptionalString(request.Args, "reason"), cancellationToken),
                "banPlayer" => await adapter.BanPlayerAsync(RequiredIdentifier(request.Args), OptionalString(request.Args, "reason"), OptionalString(request.Args, "expiresAt"), cancellationToken),
                "unbanPlayer" => await adapter.UnbanPlayerAsync(RequiredIdentifier(request.Args), cancellationToken),
                "listBans" => await adapter.ListBansAsync(cancellationToken),
                "shutdown" => await adapter.ShutdownAsync(cancellationToken),
                _ => TakaroActionResult.Fail("unsupported_action", $"CS2 connector does not support action '{request.Action}' in v1.")
            };
        }
        catch (ArgumentException ex)
        {
            return TakaroActionResult.Fail("invalid_args", ex.Message);
        }
        catch (Exception ex)
        {
            return TakaroActionResult.Fail("action_failed", ex.Message);
        }
    }

    private static string RequiredIdentifier(JsonElement args) =>
        OptionalString(args, "gameId")
        ?? OptionalString(args, "platformId")
        ?? OptionalString(args, "steamId")
        ?? OptionalString(args, "name")
        ?? (TryObject(args, "player", out var player) ? RequiredIdentifier(player) : null)
        ?? throw new ArgumentException("Expected one of gameId, platformId, steamId, or name.");

    private static string? OptionalRecipient(JsonElement args) =>
        TryObject(args, "opts", out var opts) && TryObject(opts, "recipient", out var recipient)
            ? RequiredIdentifier(recipient) : null;

    private static string? OptionalSender(JsonElement args) =>
        TryObject(args, "opts", out var opts) ? OptionalString(opts, "senderNameOverride") : null;

    private static string RequiredItemCode(JsonElement args) =>
        OptionalString(args, "code")
        ?? OptionalString(args, "name")
        ?? OptionalString(args, "itemId")
        ?? OptionalString(args, "item")
        ?? (TryObject(args, "item", out var item) ? OptionalString(item, "code") ?? OptionalString(item, "name") : null)
        ?? throw new ArgumentException("Expected an item code in code, name, itemId, or item.code.");

    private static TakaroPosition RequiredPosition(JsonElement args)
    {
        var position = TryObject(args, "position", out var nested) ? nested : args;
        static double Number(JsonElement value, string key) =>
            value.TryGetProperty(key, out var member) && member.TryGetDouble(out var number) && double.IsFinite(number)
                ? number : throw new ArgumentException($"Expected finite position.{key}.");
        return new TakaroPosition(Number(position, "x"), Number(position, "y"), Number(position, "z"));
    }

    private static int? OptionalPositiveInt(JsonElement args, string key)
    {
        if (args.ValueKind != JsonValueKind.Object || !args.TryGetProperty(key, out var value)) return null;
        if (!value.TryGetInt32(out var amount) || amount < 1 || amount > 10)
            throw new ArgumentException($"Expected {key} from 1 to 10.");
        return amount;
    }

    private static int RequiredMapZoom(JsonElement args) =>
        args.ValueKind == JsonValueKind.Object && args.TryGetProperty("z", out _)
            ? RequiredMapInt(args, "z") : RequiredMapInt(args, "zoom");

    private static int RequiredMapInt(JsonElement args, string key)
    {
        if (args.ValueKind != JsonValueKind.Object || !args.TryGetProperty(key, out var value))
            throw new ArgumentException($"Expected integer map argument '{key}'.");
        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number)) return number;
        if (value.ValueKind == JsonValueKind.String && int.TryParse(value.GetString(), System.Globalization.NumberStyles.Integer,
                System.Globalization.CultureInfo.InvariantCulture, out number)) return number;
        throw new ArgumentException($"Expected integer map argument '{key}'.");
    }

    private static bool TryObject(JsonElement args, string key, out JsonElement value)
    {
        value = default;
        return args.ValueKind == JsonValueKind.Object && args.TryGetProperty(key, out value) && value.ValueKind == JsonValueKind.Object;
    }

    private static string RequiredString(JsonElement args, string property) =>
        OptionalString(args, property) ?? throw new ArgumentException($"Expected string argument '{property}'.");

    private static string? OptionalString(JsonElement args, string property)
    {
        if (args.ValueKind != JsonValueKind.Object || !args.TryGetProperty(property, out var value) || value.ValueKind != JsonValueKind.String)
        {
            return null;
        }

        var text = value.GetString()?.Trim();
        return string.IsNullOrEmpty(text) ? null : text;
    }
}
