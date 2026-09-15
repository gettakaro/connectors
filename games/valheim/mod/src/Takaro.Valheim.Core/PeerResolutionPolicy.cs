namespace Takaro.Valheim.Core;

public sealed record PeerResolutionCandidate<TSource>(
    TSource Source,
    long PeerUid,
    bool IsReady,
    string? CharacterId,
    string? HostName,
    TakaroPlayer Player);

public static class PeerResolutionPolicy
{
    public static bool TryAssociate<TSource>(
        IEnumerable<PeerResolutionCandidate<TSource>>? candidates,
        string? characterId,
        IEnumerable<string?>? stableIdentifiers,
        IEnumerable<string?>? names,
        out PeerResolutionCandidate<TSource>? candidate,
        out bool ambiguous)
    {
        candidate = null;
        ambiguous = false;

        var readyCandidates = candidates?
            .Where(candidate => candidate is not null && candidate.IsReady)
            .ToArray()
            ?? Array.Empty<PeerResolutionCandidate<TSource>>();
        if (readyCandidates.Length == 0)
        {
            return false;
        }

        if (!string.IsNullOrWhiteSpace(characterId))
        {
            var characterMatches = readyCandidates
                .Where(current => Matches(current.CharacterId, characterId))
                .Take(2)
                .ToArray();
            if (TrySelectTier(characterMatches, out candidate, out ambiguous))
            {
                return true;
            }

            if (ambiguous)
            {
                return false;
            }
        }

        var targetStableIdentifiers = Normalize(stableIdentifiers);
        if (targetStableIdentifiers.Length > 0)
        {
            var stableMatches = readyCandidates
                .Where(current => HasAnyStableIdentifier(current, targetStableIdentifiers))
                .Take(2)
                .ToArray();
            if (TrySelectTier(stableMatches, out candidate, out ambiguous))
            {
                return true;
            }

            if (ambiguous)
            {
                return false;
            }
        }

        var targetNames = Normalize(names);
        if (targetNames.Length == 0)
        {
            return false;
        }

        var nameMatches = readyCandidates
            .Where(current => targetNames.Any(name => Matches(current.Player.Name, name)))
            .Take(2)
            .ToArray();
        return TrySelectTier(nameMatches, out candidate, out ambiguous);
    }

    public static bool TryResolveReadySender<TSource>(
        IEnumerable<PeerResolutionCandidate<TSource>>? candidates,
        long sender,
        out PeerResolutionCandidate<TSource>? candidate)
    {
        candidate = null;
        var senderMatches = candidates?
            .Where(current => current is not null && current.PeerUid == sender)
            .Take(2)
            .ToArray()
            ?? Array.Empty<PeerResolutionCandidate<TSource>>();
        if (senderMatches.Length != 1 || !senderMatches[0].IsReady)
        {
            return false;
        }

        candidate = senderMatches[0];
        return true;
    }

    private static bool HasAnyStableIdentifier<TSource>(
        PeerResolutionCandidate<TSource> candidate,
        IReadOnlyCollection<string> targetIdentifiers)
    {
        var candidateIdentifiers = Normalize(new[]
        {
            candidate.HostName,
            candidate.Player.GameId,
            candidate.Player.PlatformId,
            candidate.Player.SteamId
        });
        return candidateIdentifiers.Any(candidateIdentifier =>
            targetIdentifiers.Any(targetIdentifier => Matches(candidateIdentifier, targetIdentifier)));
    }

    private static bool TrySelectTier<TSource>(
        IReadOnlyCollection<PeerResolutionCandidate<TSource>> matches,
        out PeerResolutionCandidate<TSource>? candidate,
        out bool ambiguous)
    {
        ambiguous = matches.Count > 1;
        candidate = matches.Count == 1 ? matches.First() : null;
        return candidate is not null;
    }

    private static string[] Normalize(IEnumerable<string?>? values) =>
        values?
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .Select(value => value!.Trim())
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray()
        ?? Array.Empty<string>();

    private static bool Matches(string? value, string? expected) =>
        !string.IsNullOrWhiteSpace(value)
        && !string.IsNullOrWhiteSpace(expected)
        && value!.Trim().Equals(expected!.Trim(), StringComparison.OrdinalIgnoreCase);
}
