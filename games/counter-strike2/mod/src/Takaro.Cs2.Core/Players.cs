using System.Text.Json.Serialization;

namespace Takaro.Cs2.Core;

public sealed record Cs2Player(
    string Name,
    ulong SteamId64,
    string? Ip,
    int? Ping,
    bool IsBot,
    int ProfileRank = 0);

public sealed record TakaroPlayer(
    [property: JsonPropertyName("gameId")] string GameId,
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("steamId")] string? SteamId,
    [property: JsonPropertyName("platformId")] string? PlatformId,
    [property: JsonPropertyName("ip")] string? Ip,
    [property: JsonPropertyName("ping")] int? Ping,
    [property: JsonPropertyName("online"), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] bool? Online = null);

public static class PlayerMapper
{
    public static TakaroPlayer ToTakaroPlayer(Cs2Player player)
    {
        var steamId = player.SteamId64.ToString();
        return new TakaroPlayer(
            GameId: steamId,
            Name: player.Name,
            SteamId: steamId,
            PlatformId: $"steam:{steamId}",
            Ip: WithoutPort(player.Ip),
            Ping: player.Ping);
    }

    // CounterStrikeSharp reports the client address as host:port; Takaro expects the bare IP.
    private static string? WithoutPort(string? address)
    {
        if (string.IsNullOrWhiteSpace(address)) return null;
        var separator = address.LastIndexOf(':');
        return separator > 0 && address.IndexOf(':') == separator ? address[..separator] : address;
    }

    public static IReadOnlyList<TakaroPlayer> ToTakaroPlayers(IEnumerable<Cs2Player> players, bool includeBots) =>
        players
            .Where(player => includeBots || !player.IsBot)
            .Select(ToTakaroPlayer)
            .ToList();

    public static TakaroPlayer? Find(IEnumerable<TakaroPlayer> players, string? identifier)
    {
        if (string.IsNullOrWhiteSpace(identifier))
        {
            return null;
        }

        var needle = identifier.Trim();
        return players.FirstOrDefault(player =>
            Matches(player.GameId, needle)
            || Matches(player.PlatformId, needle)
            || Matches(player.SteamId, needle)
            || Matches(player.Name, needle));
    }

    private static bool Matches(string? value, string needle) =>
        value is not null && value.Equals(needle, StringComparison.OrdinalIgnoreCase);
}
