using System.Text.Json.Serialization;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

public sealed record TakaroProgressionTrack(
    [property: JsonPropertyName("key")] string Key,
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("level")] int Level,
    [property: JsonPropertyName("experience")] double Experience,
    [property: JsonPropertyName("progress")] double Progress);

/// <summary>
/// Maps the skills a client reports to Takaro tracks. The server owns the key and the name, so
/// a client cannot invent a track or send a key that fails the whole reading; a skill outside
/// this table (a modded one) is left out.
/// </summary>
public static class ValheimSkillTracks
{
    // Skills.SkillType name to the game's English "$skill_..." text. The key is the lowercase
    // name, the same one the game uses for that token.
    private static readonly IReadOnlyDictionary<string, string> Names = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        ["Swords"] = "Swords",
        ["Knives"] = "Knives",
        ["Clubs"] = "Clubs",
        ["Polearms"] = "Polearms",
        ["Spears"] = "Spears",
        ["Blocking"] = "Blocking",
        ["Axes"] = "Axes",
        ["Bows"] = "Bows",
        ["ElementalMagic"] = "Elemental Magic",
        ["BloodMagic"] = "Blood Magic",
        ["Unarmed"] = "Fists",
        ["Pickaxes"] = "Pickaxes",
        ["WoodCutting"] = "Wood Cutting",
        ["Crossbows"] = "Crossbows",
        ["Jump"] = "Jump",
        ["Sneak"] = "Sneak",
        ["Run"] = "Run",
        ["Swim"] = "Swim",
        ["Fishing"] = "Fishing",
        ["Cooking"] = "Cooking",
        ["Farming"] = "Farming",
        ["Crafting"] = "Crafting",
        ["Dodge"] = "Dodge",
        ["Ride"] = "Riding"
    };

    /// <summary>
    /// Level-ups come only from the accumulator, and a death leaves a fraction on the level that
    /// says nothing about the next one. So <c>progress</c> is the bar on the skills screen, not
    /// the fractional part of the level, and <c>experience</c> is the accumulator, which restarts
    /// at each level because the game keeps no running total.
    /// </summary>
    public static IReadOnlyList<TakaroProgressionTrack> ToTracks(IReadOnlyList<CompanionSkill> skills) =>
        skills
            .Where(skill => Names.ContainsKey(skill.Code))
            .Select(skill => new TakaroProgressionTrack(
                skill.Code.ToLowerInvariant(),
                Names[skill.Code],
                (int)Math.Floor(skill.Level),
                Math.Round((double)skill.Accumulator, 4),
                Math.Round((double)skill.Progress, 4)))
            .OrderBy(track => track.Key, StringComparer.Ordinal)
            .ToArray();
}

/// <summary>
/// The last skill reading of each connected player. Written on the Unity main thread and read on
/// the connector's socket thread, so getPlayerProgression never waits for the game loop.
/// </summary>
public sealed class CompanionProgressionCache
{
    private static readonly IReadOnlyList<TakaroProgressionTrack> NoTracks = Array.Empty<TakaroProgressionTrack>();

    private readonly Dictionary<long, PeerEntry> peers = new();
    private readonly object syncRoot = new();

    /// <summary>Records a connected player, who then reads as known even without the mod.</summary>
    public void ObservePeer(long peerId, TakaroPlayer player)
    {
        if (player is null)
        {
            throw new ArgumentNullException(nameof(player));
        }

        lock (syncRoot)
        {
            peers[peerId] = peers.TryGetValue(peerId, out var current)
                ? current with { Aliases = Aliases(player) }
                : new PeerEntry(Aliases(player), NoTracks, CharacterListed: false);
        }
    }

    public void Remember(long peerId, TakaroPlayer player, IReadOnlyList<CompanionSkill> skills)
    {
        if (player is null)
        {
            throw new ArgumentNullException(nameof(player));
        }

        var tracks = ValheimSkillTracks.ToTracks(skills);
        lock (syncRoot)
        {
            var listed = peers.TryGetValue(peerId, out var current) && current.CharacterListed;
            peers[peerId] = new PeerEntry(Aliases(player), tracks, listed);
        }
    }

    /// <summary>
    /// Notes which players the player list names a character for. A reading without a character
    /// beside it would land on whichever character Takaro last had active, so until then the
    /// tracks stay back.
    /// </summary>
    public void ObservePlayerList(IEnumerable<TakaroPlayer> players)
    {
        var listed = new HashSet<string>(
            players.Where(player => player.Character is not null).Select(player => player.GameId),
            StringComparer.OrdinalIgnoreCase);
        lock (syncRoot)
        {
            foreach (var peerId in peers.Keys.ToArray())
            {
                var current = peers[peerId];
                peers[peerId] = current with { CharacterListed = current.Aliases.Overlaps(listed) };
            }
        }
    }

    /// <summary>
    /// The tracks of the player's current character, an empty list for a connected player without
    /// a reading, or null for a player who is not connected.
    /// </summary>
    public IReadOnlyList<TakaroProgressionTrack>? Read(string identifier)
    {
        lock (syncRoot)
        {
            var peer = peers.Values.FirstOrDefault(candidate => candidate.Aliases.Contains(identifier));
            return peer is null ? null : peer.CharacterListed ? peer.Tracks : NoTracks;
        }
    }

    /// <summary>
    /// Drops the reading when a companion session ends, so a lapsed session cannot keep
    /// answering. The player stays known.
    /// </summary>
    public void ForgetReading(long peerId)
    {
        lock (syncRoot)
        {
            if (peers.TryGetValue(peerId, out var current))
            {
                peers[peerId] = current with { Tracks = NoTracks };
            }
        }
    }

    public void RemovePeer(long peerId)
    {
        lock (syncRoot)
        {
            peers.Remove(peerId);
        }
    }

    public void Clear()
    {
        lock (syncRoot)
        {
            peers.Clear();
        }
    }

    private static HashSet<string> Aliases(TakaroPlayer player) =>
        new(
            new[] { player.GameId, player.PlatformId, player.SteamId }
                .Where(alias => !string.IsNullOrWhiteSpace(alias))
                .Select(alias => alias!),
            StringComparer.OrdinalIgnoreCase);

    private sealed record PeerEntry(HashSet<string> Aliases, IReadOnlyList<TakaroProgressionTrack> Tracks, bool CharacterListed);
}
