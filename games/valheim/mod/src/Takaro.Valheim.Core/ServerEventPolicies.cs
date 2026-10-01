namespace Takaro.Valheim.Core;

/// <summary>Where a routed RPC packet came from, as seen by the dedicated server.</summary>
public readonly record struct RoutedPacketOrigin(
    long ArrivalPeerUid,
    long ClaimedSenderPeerId,
    string? ArrivalCharacterZdo,
    string? TargetZdo);

public enum RoutedBindingResult
{
    Bound,
    NoArrivalPeer,
    SenderMismatch,
    CharacterMismatch
}

/// <summary>
/// A routed RPC carries a sender id the client wrote itself. It only counts as that player's
/// action when the claimed sender equals the authenticated peer the packet arrived on, and,
/// for character-scoped RPCs, the target object is that peer's own character.
/// </summary>
public static class RoutedPacketBindingPolicy
{
    public static RoutedBindingResult Evaluate(RoutedPacketOrigin origin, bool requireOwnCharacter)
    {
        if (origin.ArrivalPeerUid == 0)
        {
            return RoutedBindingResult.NoArrivalPeer;
        }

        if (origin.ClaimedSenderPeerId != origin.ArrivalPeerUid)
        {
            return RoutedBindingResult.SenderMismatch;
        }

        if (requireOwnCharacter
            && (string.IsNullOrWhiteSpace(origin.ArrivalCharacterZdo)
                || !string.Equals(origin.ArrivalCharacterZdo, origin.TargetZdo, StringComparison.Ordinal)))
        {
            return RoutedBindingResult.CharacterMismatch;
        }

        return RoutedBindingResult.Bound;
    }
}

public enum ValheimTalkerType
{
    Whisper = 0,
    Normal = 1,
    Shout = 2,
    Ping = 3
}

public readonly record struct ChatDecision(bool Emit, string Reason, string Text);

/// <summary>
/// Decides whether one observed chat packet becomes a Takaro chat-message. A Valheim client
/// sends one copy of every line per player-list entry, so several copies of the same line can
/// pass through the server; only the first bound copy inside the window is emitted.
/// </summary>
public sealed class ServerChatRelayPolicy
{
    public const int MaximumMessageCharacters = 1024;
    public static readonly TimeSpan DuplicateWindow = TimeSpan.FromSeconds(3);

    private readonly Dictionary<string, DateTimeOffset> recent = new(StringComparer.Ordinal);
    private readonly object syncRoot = new();

    public ChatDecision Evaluate(
        RoutedPacketOrigin origin,
        bool characterScoped,
        int talkerType,
        string? text,
        DateTimeOffset now)
    {
        var binding = RoutedPacketBindingPolicy.Evaluate(origin, characterScoped);
        if (binding != RoutedBindingResult.Bound)
        {
            return new ChatDecision(false, $"unbound:{binding}", string.Empty);
        }

        if (talkerType is < (int)ValheimTalkerType.Whisper or > (int)ValheimTalkerType.Shout)
        {
            return new ChatDecision(false, "not-a-chat-line", string.Empty);
        }

        var trimmed = text?.Trim() ?? string.Empty;
        if (trimmed.Length == 0)
        {
            return new ChatDecision(false, "empty", string.Empty);
        }

        if (trimmed.Length > MaximumMessageCharacters)
        {
            return new ChatDecision(false, "too-long", string.Empty);
        }

        var key = $"{origin.ArrivalPeerUid}|{talkerType}|{trimmed}";
        lock (syncRoot)
        {
            Prune(now);
            if (recent.TryGetValue(key, out var seenAt) && now - seenAt < DuplicateWindow)
            {
                return new ChatDecision(false, "duplicate-copy", trimmed);
            }

            recent[key] = now;
        }

        return new ChatDecision(true, "bound", trimmed);
    }

    private void Prune(DateTimeOffset now)
    {
        if (recent.Count < 256)
        {
            return;
        }

        foreach (var stale in recent.Where(entry => now - entry.Value >= DuplicateWindow).Select(entry => entry.Key).ToArray())
        {
            recent.Remove(stale);
        }
    }
}

/// <summary>Collapses the two server paths that see one routed death packet into one event.</summary>
public sealed class OnceWithinWindow
{
    private readonly TimeSpan window;
    private readonly Dictionary<string, DateTimeOffset> seen = new(StringComparer.Ordinal);
    private readonly object syncRoot = new();

    public OnceWithinWindow(TimeSpan window)
    {
        this.window = window;
    }

    public bool TryAccept(string key, DateTimeOffset now)
    {
        lock (syncRoot)
        {
            if (seen.Count > 512)
            {
                foreach (var stale in seen.Where(entry => now - entry.Value >= window).Select(entry => entry.Key).ToArray())
                {
                    seen.Remove(stale);
                }
            }

            if (seen.TryGetValue(key, out var at) && now - at < window)
            {
                return false;
            }

            seen[key] = now;
            return true;
        }
    }
}

public readonly record struct KillCandidate(long PeerUid, bool HitTheCreature);

public readonly record struct KillAttribution(long? PeerUid, string Reason);

/// <summary>
/// Valheim marks every player that damaged a creature on the creature's own network object
/// ("Attackers" + player name). When the creature's object is destroyed, the server picks the
/// killer from those marks: the player whose game destroyed it if that player hit it, else the
/// only player that hit it. Several hitters and none of them the destroyer is left ambiguous.
/// </summary>
public static class KillAttributionPolicy
{
    public static KillAttribution Choose(IReadOnlyCollection<KillCandidate> candidates, long destroyingPeerUid)
    {
        var hitters = candidates.Where(candidate => candidate.HitTheCreature).Select(candidate => candidate.PeerUid).Distinct().ToArray();
        if (hitters.Length == 0)
        {
            return new KillAttribution(null, "no-player-hit");
        }

        if (hitters.Contains(destroyingPeerUid))
        {
            return new KillAttribution(destroyingPeerUid, "destroyer-hit");
        }

        return hitters.Length == 1
            ? new KillAttribution(hitters[0], "single-hitter")
            : new KillAttribution(null, "ambiguous-hitters");
    }
}

public enum ValheimKillModifier
{
    MixedAndTotal = 0,
    Unarmed = 1,
    Magic = 2,
    Ranged = 3,
    Melee = 4,
    CountNone = 5
}

/// <summary>Names the weapon of a kill from the killer's visible equipment and the kill style.</summary>
public static class KillWeaponPolicy
{
    public static string Describe(int killModifier, string? rightHandItem, string? leftHandItem)
    {
        var modifier = (ValheimKillModifier)killModifier;
        if (modifier == ValheimKillModifier.Unarmed)
        {
            return "Unarmed";
        }

        var preferred = modifier == ValheimKillModifier.Ranged
            ? FirstNonEmpty(leftHandItem, rightHandItem)
            : FirstNonEmpty(rightHandItem, leftHandItem);
        if (preferred is not null)
        {
            return preferred;
        }

        return modifier switch
        {
            ValheimKillModifier.Melee => "Melee",
            ValheimKillModifier.Ranged => "Ranged",
            ValheimKillModifier.Magic => "Magic",
            _ => "Unarmed"
        };
    }

    private static string? FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value))?.Trim();
}

/// <summary>Turns a Valheim localisation token into readable text when no translation is loaded.</summary>
public static class ValheimDisplayName
{
    public static string FromToken(string? token, string fallback)
    {
        if (string.IsNullOrWhiteSpace(token))
        {
            return fallback;
        }

        var value = token!.Trim();
        return value.StartsWith("$", StringComparison.Ordinal) ? fallback : value;
    }
}

/// <summary>
/// A creature killed by a single blow never shows the server who hit it: the killer's game
/// marks the attacker and destroys the creature in the same frame, so only the destroy reaches
/// the server. Such a death still counts as a player kill when three server-visible facts line
/// up: the creature's ragdoll appears from the same game, that game's player started a weapon
/// attack (its animation trigger passes through the server) just before, and that player stood
/// close enough to have dealt the blow.
/// </summary>
public static class InferredKillPolicy
{
    public const float MaximumKillerDistance = 20f;
    public static readonly TimeSpan EvidenceWindow = TimeSpan.FromSeconds(8);
    public static readonly TimeSpan AttackLeadWindow = TimeSpan.FromSeconds(4);

    public static bool KillerCloseEnough(float distance) =>
        !float.IsNaN(distance) && distance >= 0 && distance <= MaximumKillerDistance;

    public static bool StillWaiting(DateTimeOffset destroyedAt, DateTimeOffset now) =>
        now - destroyedAt < EvidenceWindow;

    public static bool AttackedJustBefore(DateTimeOffset? lastAttackAt, DateTimeOffset destroyedAt) =>
        lastAttackAt is { } attack
        && attack <= destroyedAt + TimeSpan.FromMilliseconds(500)
        && destroyedAt - attack <= AttackLeadWindow;

    /// <summary>Valheim appends the combo step to attack triggers ("swing_longsword2").</summary>
    public static string AttackAnimationBase(string? trigger)
    {
        if (string.IsNullOrWhiteSpace(trigger))
        {
            return string.Empty;
        }

        var value = trigger!.Trim();
        var end = value.Length;
        while (end > 0 && char.IsDigit(value[end - 1]))
        {
            end--;
        }

        return value.Substring(0, end);
    }
}
