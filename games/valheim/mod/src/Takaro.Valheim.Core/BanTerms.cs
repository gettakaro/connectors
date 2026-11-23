using System.Text.Json;
using System.Text.Json.Serialization;

namespace Takaro.Valheim.Core;

/// <summary>The reason and end of one ban, as Takaro sent them.</summary>
public sealed record BanTerm(
    [property: JsonPropertyName("reason")] string Reason,
    [property: JsonPropertyName("expiresAt")] DateTimeOffset? ExpiresAt);

/// <summary>
/// Reason and expiry per banned player, persisted across restarts. Valheim's ban list holds ids
/// only, so without this a timed ban from Takaro would never end and listBans could not report
/// what the operator set.
/// </summary>
public sealed class BanTerms
{
    private readonly string? path;
    private readonly object gate = new();
    private readonly Dictionary<string, BanTerm> terms = new(StringComparer.OrdinalIgnoreCase);

    public BanTerms(string? path = null)
    {
        this.path = path;
    }

    /// <summary>Reads the persisted terms; a missing file starts empty.</summary>
    public void Load()
    {
        if (path is null || !File.Exists(path))
        {
            return;
        }

        var stored = JsonSerializer.Deserialize<Dictionary<string, BanTerm>>(File.ReadAllText(path));
        lock (gate)
        {
            foreach (var entry in stored ?? new Dictionary<string, BanTerm>())
            {
                if (!string.IsNullOrWhiteSpace(entry.Key) && entry.Value is not null)
                {
                    terms[Key(entry.Key)] = entry.Value with { Reason = entry.Value.Reason ?? string.Empty };
                }
            }
        }
    }

    /// <summary>Stores the terms of a new ban, replacing earlier ones for the same player.</summary>
    public void Record(string gameId, string? reason, DateTimeOffset? expiresAt)
    {
        if (string.IsNullOrWhiteSpace(gameId))
        {
            return;
        }

        string snapshot;
        lock (gate)
        {
            terms[Key(gameId)] = new BanTerm(reason?.Trim() ?? string.Empty, expiresAt);
            snapshot = JsonSerializer.Serialize(terms);
        }

        Save(snapshot);
    }

    /// <summary>Drops the terms of the given players; returns how many were stored.</summary>
    public int Forget(IEnumerable<string> gameIds)
    {
        string snapshot;
        var removed = 0;
        lock (gate)
        {
            foreach (var gameId in gameIds.Where(id => !string.IsNullOrWhiteSpace(id)))
            {
                if (terms.Remove(Key(gameId)))
                {
                    removed++;
                }
            }

            if (removed == 0)
            {
                return 0;
            }

            snapshot = JsonSerializer.Serialize(terms);
        }

        Save(snapshot);
        return removed;
    }

    public bool TryGet(string? gameId, out BanTerm term)
    {
        term = new BanTerm(string.Empty, null);
        if (string.IsNullOrWhiteSpace(gameId))
        {
            return false;
        }

        lock (gate)
        {
            if (terms.TryGetValue(Key(gameId!), out var stored))
            {
                term = stored;
                return true;
            }
        }

        return false;
    }

    /// <summary>Players whose ban has ended at <paramref name="now"/>.</summary>
    public IReadOnlyList<string> Due(DateTimeOffset now)
    {
        lock (gate)
        {
            return terms
                .Where(entry => entry.Value.ExpiresAt is { } end && end <= now)
                .Select(entry => entry.Key)
                .ToArray();
        }
    }

    /// <summary>Drops terms for players the game no longer bans (an admin lifted them in game).</summary>
    public int Retain(IEnumerable<string> bannedIds)
    {
        var banned = new HashSet<string>(bannedIds.Where(id => !string.IsNullOrWhiteSpace(id)).Select(Key), StringComparer.OrdinalIgnoreCase);
        string[] stale;
        lock (gate)
        {
            stale = terms.Keys.Where(key => !banned.Contains(key)).ToArray();
        }

        return stale.Length == 0 ? 0 : Forget(stale);
    }

    private static string Key(string gameId) => PlayerMapper.ToCanonicalPlatformUserId(gameId.Trim());

    private void Save(string snapshot)
    {
        if (path is null)
        {
            return;
        }

        var temp = path + ".tmp";
        File.WriteAllText(temp, snapshot);
        if (File.Exists(path))
        {
            File.Delete(path);
        }

        File.Move(temp, path);
    }
}
