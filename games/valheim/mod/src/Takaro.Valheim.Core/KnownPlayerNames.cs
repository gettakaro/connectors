using System.Text.Json;

namespace Takaro.Valheim.Core;

/// <summary>
/// Last character name seen per player, persisted across restarts. Valheim's ban list holds ids
/// only, and Takaro writes the <c>name</c> of every listBans player onto the matched profile, so a
/// ban entry without a known name would rename the player to its id.
/// </summary>
public sealed class KnownPlayerNames
{
    private readonly string? path;
    private readonly object gate = new();
    private readonly Dictionary<string, string> names = new(StringComparer.OrdinalIgnoreCase);

    public KnownPlayerNames(string? path = null)
    {
        this.path = path;
    }

    /// <summary>Reads the persisted names; a missing or unreadable file starts empty.</summary>
    public void Load()
    {
        if (path is null || !File.Exists(path))
        {
            return;
        }

        var stored = JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(path));
        lock (gate)
        {
            foreach (var entry in stored ?? new Dictionary<string, string>())
            {
                if (!string.IsNullOrWhiteSpace(entry.Key) && !string.IsNullOrWhiteSpace(entry.Value))
                {
                    names[PlayerMapper.ToCanonicalPlatformUserId(entry.Key)] = entry.Value;
                }
            }
        }
    }

    /// <summary>Records a name; writes the file only when the stored name changed.</summary>
    public void Observe(string? gameId, string? name)
    {
        if (string.IsNullOrWhiteSpace(gameId) || string.IsNullOrWhiteSpace(name)
            || string.Equals(gameId, name, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        var key = PlayerMapper.ToCanonicalPlatformUserId(gameId!);
        string snapshot;
        lock (gate)
        {
            if (names.TryGetValue(key, out var existing) && existing == name)
            {
                return;
            }

            names[key] = name!.Trim();
            snapshot = JsonSerializer.Serialize(names);
        }

        if (path is not null)
        {
            var temp = path + ".tmp";
            File.WriteAllText(temp, snapshot);
            if (File.Exists(path))
            {
                File.Delete(path);
            }

            File.Move(temp, path);
        }
    }

    public bool TryGet(string? gameId, out string name)
    {
        name = string.Empty;
        if (string.IsNullOrWhiteSpace(gameId))
        {
            return false;
        }

        lock (gate)
        {
            if (names.TryGetValue(PlayerMapper.ToCanonicalPlatformUserId(gameId!), out var found))
            {
                name = found;
                return true;
            }
        }

        return false;
    }
}
