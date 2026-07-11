using System.Collections.ObjectModel;
using System.Globalization;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

public enum CompanionInventoryState
{
    Fresh,
    Missing,
    Expired
}

public sealed class CompanionInventoryCache
{
    private static readonly IReadOnlyList<TakaroInventoryItem> UnavailableItems =
        new ReadOnlyCollection<TakaroInventoryItem>(Array.Empty<TakaroInventoryItem>());

    private readonly TimeSpan freshness;
    private readonly Dictionary<long, PeerState> peers = new();
    private readonly Dictionary<string, HashSet<Observation>> stableObservationsByAlias =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, HashSet<Observation>> secondaryObservationsByAlias =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly object syncRoot = new();
    private object? currentWorldIdentity;
    private bool hasCurrentWorldIdentity;

    public CompanionInventoryCache()
        : this(TimeSpan.FromSeconds(30))
    {
    }

    public CompanionInventoryCache(TimeSpan freshness)
    {
        if (freshness <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(freshness), "Inventory freshness must be positive.");
        }

        this.freshness = freshness;
    }

    public void BeginSession(long peerId, string sessionNonce)
    {
        ValidateSessionNonce(sessionNonce);

        lock (syncRoot)
        {
            if (peers.TryGetValue(peerId, out var current)
                && string.Equals(current.SessionNonce, sessionNonce, StringComparison.Ordinal))
            {
                return;
            }

            if (current is not null)
            {
                RemoveObservationAliasesLocked(current.Observation);
            }

            peers[peerId] = new PeerState(sessionNonce);
        }
    }

    public bool Remember(
        long peerId,
        string sessionNonce,
        TakaroPlayer player,
        IReadOnlyList<CompanionInventoryStack> stacks,
        DateTimeOffset observedAt)
    {
        if (!IsValidSessionNonce(sessionNonce))
        {
            return false;
        }

        PeerState expectedSession;
        lock (syncRoot)
        {
            if (!peers.TryGetValue(peerId, out expectedSession!)
                || !string.Equals(expectedSession.SessionNonce, sessionNonce, StringComparison.Ordinal))
            {
                return false;
            }
        }

        if (!TryCanonicalize(
            player,
            stacks,
            out var stableAliases,
            out var secondaryAliases,
            out var items))
        {
            return false;
        }

        lock (syncRoot)
        {
            if (!peers.TryGetValue(peerId, out var currentSession)
                || !ReferenceEquals(currentSession, expectedSession)
                || !string.Equals(currentSession.SessionNonce, sessionNonce, StringComparison.Ordinal))
            {
                return false;
            }

            if (currentSession.Observation is not null
                && observedAt < currentSession.Observation.ObservedAt)
            {
                return false;
            }

            RemoveObservationAliasesLocked(currentSession.Observation);
            var observation = new Observation(
                peerId,
                sessionNonce,
                stableAliases,
                secondaryAliases,
                items,
                observedAt);
            currentSession.Observation = observation;
            AddObservationAliasesLocked(observation);

            return true;
        }
    }

    public CompanionInventoryState TryGet(
        string identifier,
        DateTimeOffset now,
        out IReadOnlyList<TakaroInventoryItem> items)
    {
        items = UnavailableItems;
        if (string.IsNullOrWhiteSpace(identifier))
        {
            return CompanionInventoryState.Missing;
        }

        lock (syncRoot)
        {
            var normalizedIdentifier = identifier.Trim();
            if (stableObservationsByAlias.TryGetValue(
                normalizedIdentifier,
                out var stableObservations))
            {
                return stableObservations.Count == 1
                    ? QueryObservationLocked(stableObservations.First(), now, out items)
                    : CompanionInventoryState.Missing;
            }

            if (!secondaryObservationsByAlias.TryGetValue(
                    normalizedIdentifier,
                    out var secondaryObservations)
                || secondaryObservations.Count != 1)
            {
                return CompanionInventoryState.Missing;
            }

            return QueryObservationLocked(secondaryObservations.First(), now, out items);
        }
    }

    public void RemovePeer(long peerId)
    {
        lock (syncRoot)
        {
            if (!peers.TryGetValue(peerId, out var peer))
            {
                return;
            }

            RemoveObservationAliasesLocked(peer.Observation);
            peers.Remove(peerId);
        }
    }

    /// <summary>
    /// Switches to a caller-supplied stable, immutable world value key.
    /// Value equality defines world equivalence, so an equal key preserves current inventory.
    /// </summary>
    public void SwitchWorld(object? worldIdentity)
    {
        lock (syncRoot)
        {
            if (hasCurrentWorldIdentity && Equals(currentWorldIdentity, worldIdentity))
            {
                return;
            }

            ClearLocked();
            currentWorldIdentity = worldIdentity;
            hasCurrentWorldIdentity = true;
        }
    }

    public void Clear()
    {
        lock (syncRoot)
        {
            ClearLocked();
        }
    }

    private void ClearLocked()
    {
        stableObservationsByAlias.Clear();
        secondaryObservationsByAlias.Clear();
        peers.Clear();
    }

    private CompanionInventoryState QueryObservationLocked(
        Observation observation,
        DateTimeOffset now,
        out IReadOnlyList<TakaroInventoryItem> items)
    {
        items = UnavailableItems;
        if (observation.IsExpired)
        {
            return CompanionInventoryState.Expired;
        }

        if (now >= observation.ObservedAt
            && now - observation.ObservedAt >= freshness)
        {
            observation.MarkExpired(UnavailableItems);
            return CompanionInventoryState.Expired;
        }

        items = observation.Items;
        return CompanionInventoryState.Fresh;
    }

    private void AddObservationAliasesLocked(Observation observation)
    {
        AddAliasesLocked(
            stableObservationsByAlias,
            observation.StableAliases,
            observation);
        AddAliasesLocked(
            secondaryObservationsByAlias,
            observation.SecondaryAliases,
            observation);
    }

    private static void AddAliasesLocked(
        IDictionary<string, HashSet<Observation>> observationsByAlias,
        IEnumerable<string> aliases,
        Observation observation)
    {
        foreach (var alias in aliases)
        {
            if (!observationsByAlias.TryGetValue(alias, out var observations))
            {
                observations = new HashSet<Observation>();
                observationsByAlias.Add(alias, observations);
            }

            observations.Add(observation);
        }
    }

    private void RemoveObservationAliasesLocked(Observation? observation)
    {
        if (observation is null)
        {
            return;
        }

        RemoveAliasesLocked(
            stableObservationsByAlias,
            observation.StableAliases,
            observation);
        RemoveAliasesLocked(
            secondaryObservationsByAlias,
            observation.SecondaryAliases,
            observation);
    }

    private static void RemoveAliasesLocked(
        IDictionary<string, HashSet<Observation>> observationsByAlias,
        IEnumerable<string> aliases,
        Observation observation)
    {
        foreach (var alias in aliases)
        {
            if (!observationsByAlias.TryGetValue(alias, out var observations))
            {
                continue;
            }

            observations.Remove(observation);
            if (observations.Count == 0)
            {
                observationsByAlias.Remove(alias);
            }
        }
    }

    private static bool TryCanonicalize(
        TakaroPlayer player,
        IReadOnlyList<CompanionInventoryStack> stacks,
        out IReadOnlyList<string> stableAliases,
        out IReadOnlyList<string> secondaryAliases,
        out IReadOnlyList<TakaroInventoryItem> items)
    {
        stableAliases = Array.Empty<string>();
        secondaryAliases = Array.Empty<string>();
        items = UnavailableItems;
        if (player is null
            || stacks is null
            || stacks.Count > CompanionProtocol.MaximumInventoryStacks)
        {
            return false;
        }

        var resolvedStableAliases = StablePlayerAliases(player).ToArray();
        var resolvedSecondaryAliases = SecondaryPlayerAliases(player).ToArray();
        if (resolvedStableAliases.Length == 0 && resolvedSecondaryAliases.Length == 0)
        {
            return false;
        }

        var canonicalStacks = new List<CanonicalStack>(stacks.Count);
        for (var index = 0; index < stacks.Count; index++)
        {
            var stack = stacks[index];
            if (!TryCanonicalizeStack(stack, index, out var canonical))
            {
                return false;
            }

            canonicalStacks.Add(canonical);
        }

        var mapped = canonicalStacks
            .OrderBy(stack => stack.Slot)
            .ThenBy(stack => stack.Code, StringComparer.Ordinal)
            .ThenBy(stack => stack.Name, StringComparer.Ordinal)
            .ThenBy(stack => stack.OriginalIndex)
            .Select(stack => new TakaroInventoryItem(
                stack.Code,
                stack.Name,
                stack.Amount,
                stack.Quality.ToString(CultureInfo.InvariantCulture),
                stack.Durability,
                stack.Equipped,
                new TakaroInventorySlot(stack.Slot, 0)))
            .ToArray();

        stableAliases = Array.AsReadOnly(resolvedStableAliases);
        secondaryAliases = Array.AsReadOnly(resolvedSecondaryAliases);
        items = Array.AsReadOnly(mapped);
        return true;
    }

    private static bool TryCanonicalizeStack(
        CompanionInventoryStack stack,
        int originalIndex,
        out CanonicalStack canonical)
    {
        canonical = default!;
        if (stack is null
            || string.IsNullOrWhiteSpace(stack.Code)
            || stack.Code.Length > CompanionProtocol.MaximumCodeCharacters
            || string.IsNullOrWhiteSpace(stack.Name)
            || stack.Name.Length > CompanionProtocol.MaximumChatCharacters
            || stack.Amount <= 0
            || stack.Amount > CompanionProtocol.MaximumInventoryAmount
            || stack.Quality <= 0
            || stack.Quality > CompanionProtocol.MaximumItemQuality
            || float.IsNaN(stack.Durability)
            || float.IsInfinity(stack.Durability)
            || stack.Durability < 0
            || stack.Durability > CompanionProtocol.MaximumDurability
            || stack.Slot < 0
            || stack.Slot > CompanionProtocol.MaximumInventorySlot)
        {
            return false;
        }

        canonical = new CanonicalStack(
            stack.Code.Trim(),
            stack.Name.Trim(),
            stack.Amount,
            stack.Quality,
            stack.Durability,
            stack.Equipped,
            stack.Slot,
            originalIndex);
        return true;
    }

    private static IEnumerable<string> StablePlayerAliases(TakaroPlayer player) =>
        NormalizeAliases(new[] { player.GameId, player.PlatformId, player.SteamId });

    private static IEnumerable<string> SecondaryPlayerAliases(TakaroPlayer player) =>
        NormalizeAliases(new[] { player.Name });

    private static IEnumerable<string> NormalizeAliases(IEnumerable<string?> aliases) =>
        aliases
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .Select(value => value!.Trim())
            .Distinct(StringComparer.OrdinalIgnoreCase);

    private static void ValidateSessionNonce(string sessionNonce)
    {
        if (!IsValidSessionNonce(sessionNonce))
        {
            throw new ArgumentException(
                $"Session nonce must contain 1 to {CompanionEnvelopeCodec.MaximumSessionNonceCharacters} characters.",
                nameof(sessionNonce));
        }
    }

    private static bool IsValidSessionNonce(string? sessionNonce) =>
        !string.IsNullOrWhiteSpace(sessionNonce)
        && sessionNonce!.Length <= CompanionEnvelopeCodec.MaximumSessionNonceCharacters;

    private sealed class PeerState
    {
        public PeerState(string sessionNonce)
        {
            SessionNonce = sessionNonce;
        }

        public string SessionNonce { get; }

        public Observation? Observation { get; set; }
    }

    private sealed class Observation
    {
        public Observation(
            long peerId,
            string sessionNonce,
            IReadOnlyList<string> stableAliases,
            IReadOnlyList<string> secondaryAliases,
            IReadOnlyList<TakaroInventoryItem> items,
            DateTimeOffset observedAt)
        {
            PeerId = peerId;
            SessionNonce = sessionNonce;
            StableAliases = stableAliases;
            SecondaryAliases = secondaryAliases;
            Items = items;
            ObservedAt = observedAt;
        }

        public long PeerId { get; }

        public string SessionNonce { get; }

        public IReadOnlyList<string> StableAliases { get; }

        public IReadOnlyList<string> SecondaryAliases { get; }

        public IReadOnlyList<TakaroInventoryItem> Items { get; private set; }

        public DateTimeOffset ObservedAt { get; }

        public bool IsExpired { get; private set; }

        public void MarkExpired(IReadOnlyList<TakaroInventoryItem> unavailableItems)
        {
            Items = unavailableItems;
            IsExpired = true;
        }
    }

    private sealed class CanonicalStack
    {
        public CanonicalStack(
            string code,
            string name,
            int amount,
            int quality,
            float durability,
            bool equipped,
            int slot,
            int originalIndex)
        {
            Code = code;
            Name = name;
            Amount = amount;
            Quality = quality;
            Durability = durability;
            Equipped = equipped;
            Slot = slot;
            OriginalIndex = originalIndex;
        }

        public string Code { get; }

        public string Name { get; }

        public int Amount { get; }

        public int Quality { get; }

        public float Durability { get; }

        public bool Equipped { get; }

        public int Slot { get; }

        public int OriginalIndex { get; }
    }
}
