using System.Text.Json;

namespace Takaro.Cs2.Core;

public sealed record Cs2Ban(string SteamId64, string Name, string Reason, DateTimeOffset? ExpiresAt);

public sealed class Cs2BanStore
{
    private readonly string path;
    private readonly object gate = new();
    private List<Cs2Ban> entries;

    public Cs2BanStore(string path)
    {
        this.path = path;
        entries = File.Exists(path)
            ? JsonSerializer.Deserialize<List<Cs2Ban>>(File.ReadAllText(path)) ?? throw new InvalidDataException("Ban store is null.")
            : [];
    }

    public IReadOnlyList<Cs2Ban> Active()
    {
        lock (gate)
        {
            PruneExpired();
            return entries.ToArray();
        }
    }

    public bool IsBanned(string steamId64) => Active().Any(entry => entry.SteamId64 == steamId64);

    public void Add(string steamId64, string name, string reason, DateTimeOffset? expiresAt)
    {
        lock (gate)
        {
            entries.RemoveAll(entry => entry.SteamId64 == steamId64);
            entries.Add(new Cs2Ban(steamId64, name, reason, expiresAt));
            Save();
        }
    }

    public bool Remove(string steamId64)
    {
        lock (gate)
        {
            var removed = entries.RemoveAll(entry => entry.SteamId64 == steamId64) > 0;
            if (removed) Save();
            return removed;
        }
    }

    private void PruneExpired()
    {
        if (entries.RemoveAll(entry => entry.ExpiresAt <= DateTimeOffset.UtcNow) > 0) Save();
    }

    private void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(entries));
        File.Move(temporary, path, true);
    }
}
