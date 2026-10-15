using System.Text.Json;

namespace Takaro.Cs2.Core;

public sealed class Cs2PlayerStore
{
    private readonly string path;
    private readonly object gate = new();
    private readonly Dictionary<string, TakaroPlayer> players;

    public Cs2PlayerStore(string path)
    {
        this.path = path;
        var saved = File.Exists(path)
            ? JsonSerializer.Deserialize<List<TakaroPlayer>>(File.ReadAllText(path)) ?? throw new InvalidDataException("Player store is null.")
            : [];
        players = saved.ToDictionary(player => player.GameId, StringComparer.Ordinal);
    }

    public TakaroPlayer? Find(string identifier)
    {
        lock (gate)
        {
            var known = PlayerMapper.Find(players.Values, identifier);
            if (known is not null) return known with { Online = false };
        }
        var id = identifier.StartsWith("steam:", StringComparison.OrdinalIgnoreCase) ? identifier[6..] : identifier;
        return ulong.TryParse(id, out var number) && number >= 76561197960265728UL
            ? new TakaroPlayer(id, id, id, $"steam:{id}", null, null, false)
            : null;
    }

    public void Remember(TakaroPlayer player)
    {
        lock (gate)
        {
            players[player.GameId] = player with { Ip = null, Ping = null };
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var temporary = path + ".tmp";
            File.WriteAllText(temporary, JsonSerializer.Serialize(players.Values.OrderBy(value => value.GameId)));
            File.Move(temporary, path, true);
        }
    }
}
