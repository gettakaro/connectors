using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Companion;

/// <summary>What the client saw about one creature just before it died.</summary>
public sealed class CompanionKillObservation
{
    public CompanionKillObservation(
        long zdoUserId,
        uint zdoId,
        string? prefab,
        string? enemyToken,
        bool localPlayerMarkedAsAttacker,
        string lastHitAttackerKind)
    {
        ZdoUserId = zdoUserId;
        ZdoId = zdoId;
        Prefab = prefab;
        EnemyToken = enemyToken;
        LocalPlayerMarkedAsAttacker = localPlayerMarkedAsAttacker;
        LastHitAttackerKind = lastHitAttackerKind;
    }

    public long ZdoUserId { get; }

    public uint ZdoId { get; }

    public string? Prefab { get; }

    public string? EnemyToken { get; }

    public bool LocalPlayerMarkedAsAttacker { get; }

    public string LastHitAttackerKind { get; }
}

public static class CompanionKillVerdictPolicy
{
    /// <summary>
    /// Turns an observation into a verdict, or nothing: only creatures the local player is
    /// marked on as an attacker are reported, and every field must fit the wire bounds.
    /// </summary>
    public static CompanionKillVerdict? ToVerdict(CompanionKillObservation? observation)
    {
        if (observation is null
            || !observation.LocalPlayerMarkedAsAttacker
            || (observation.ZdoUserId == 0 && observation.ZdoId == 0)
            || !CompanionAttackerKind.IsKnown(observation.LastHitAttackerKind))
        {
            return null;
        }

        var prefab = Bounded(observation.Prefab);
        var token = Bounded(observation.EnemyToken);
        if (prefab is null || token is null)
        {
            return null;
        }

        var zdo = string.Concat(
            observation.ZdoUserId.ToString(System.Globalization.CultureInfo.InvariantCulture),
            ":",
            observation.ZdoId.ToString(System.Globalization.CultureInfo.InvariantCulture));
        return new CompanionKillVerdict(
            zdo,
            prefab,
            token,
            observation.LastHitAttackerKind == CompanionAttackerKind.LocalPlayer,
            observation.LastHitAttackerKind);
    }

    private static string? Bounded(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }

        var trimmed = value!.Trim();
        return trimmed.Length <= CompanionProtocol.MaximumCodeCharacters ? trimmed : null;
    }
}
