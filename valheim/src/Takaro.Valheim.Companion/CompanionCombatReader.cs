using System.Globalization;
using Takaro.Valheim.Companion.Protocol;

#if TAKARO_VALHEIM_COMPANION
using HarmonyLib;
#endif

namespace Takaro.Valheim.Companion;

public sealed class CompanionCombatReader
{
    public static readonly TimeSpan DuplicateWindow = TimeSpan.FromSeconds(2);

    private const int MaximumRecentEvents = 1024;
    private readonly Dictionary<string, TimeSpan> recentEvents =
        new(StringComparer.Ordinal);
    private readonly Queue<RecentEvent> recentOrder = new();

    public bool TryReserveEvent(
        string eventKind,
        string sourceId,
        TimeSpan monotonicNow,
        out string? eventId)
    {
        eventId = null;
        if ((eventKind != CompanionMessageTypes.PlayerDeath
                && eventKind != CompanionMessageTypes.EntityKilled)
            || string.IsNullOrWhiteSpace(sourceId)
            || sourceId.Length > CompanionProtocol.MaximumCodeCharacters
            || monotonicNow < TimeSpan.Zero)
        {
            return false;
        }

        Trim(monotonicNow);
        var key = eventKind + "\0" + sourceId;
        if (recentEvents.TryGetValue(key, out var previous)
            && monotonicNow < SaturatingAdd(previous, DuplicateWindow))
        {
            return false;
        }

        recentEvents[key] = monotonicNow;
        recentOrder.Enqueue(new RecentEvent(key, monotonicNow));
        TrimToCapacity();
        eventId = eventKind + "-" + Guid.NewGuid().ToString("N");
        return true;
    }

    public void Reset()
    {
        recentEvents.Clear();
        recentOrder.Clear();
    }

#if TAKARO_VALHEIM_COMPANION
    private static readonly System.Reflection.FieldInfo? LastHitField =
        AccessTools.Field(typeof(Character), "m_lastHit");

    internal bool TryCreateLocalPlayerDeath(
        Player player,
        TimeSpan monotonicNow,
        DateTimeOffset utcNow,
        out CompanionPlayerDeathReport? report)
    {
        report = null;
        if (player == null
            || player != Player.m_localPlayer
            || !TryPosition(player.transform.position, out var position)
            || !TryReserveEvent(
                CompanionMessageTypes.PlayerDeath,
                player.GetInstanceID().ToString(CultureInfo.InvariantCulture),
                monotonicNow,
                out var eventId)
            || eventId is null)
        {
            return false;
        }

        var hit = GetLastHit(player);
        var attacker = hit?.GetAttacker();
        report = new CompanionPlayerDeathReport(
            eventId,
            utcNow.ToUnixTimeMilliseconds(),
            position,
            WeaponHint(hit) ?? BoundedHint(hit?.m_skill.ToString()),
            CharacterHint(attacker));
        return true;
    }

    internal bool TryCreateEntityKilled(
        Character character,
        HitData hit,
        TimeSpan monotonicNow,
        DateTimeOffset utcNow,
        out CompanionEntityKilledReport? report)
    {
        report = null;
        if (character == null
            || character is Player
            || character.GetComponent<Player>() != null
            || hit is null
            || hit.GetAttacker() != Player.m_localPlayer
            || !TryPosition(character.transform.position, out var position)
            || !TryReserveEvent(
                CompanionMessageTypes.EntityKilled,
                character.GetInstanceID().ToString(CultureInfo.InvariantCulture),
                monotonicNow,
                out var eventId)
            || eventId is null)
        {
            return false;
        }

        report = new CompanionEntityKilledReport(
            eventId,
            utcNow.ToUnixTimeMilliseconds(),
            position,
            CharacterHint(character),
            WeaponHint(hit)
                ?? BoundedHint(hit.m_skill.ToString())
                ?? "unarmed");
        return true;
    }

    internal static HitData? GetLastHit(Character character) =>
        LastHitField?.GetValue(character) as HitData;

    private static string? WeaponHint(HitData? hit)
    {
        if (hit is null)
        {
            return null;
        }

        var attacker = hit.GetAttacker() as Humanoid;
        var weapon = attacker?.GetCurrentWeapon();
        if (weapon is null)
        {
            return null;
        }

        var prefabName = weapon.m_dropPrefab != null
            ? weapon.m_dropPrefab.name
            : null;
        var displayName = weapon.m_shared?.m_name;
        return BoundedHint(prefabName)
            ?? BoundedHint(displayName);
    }

    private static string? CharacterHint(Character? character)
    {
        if (character == null)
        {
            return null;
        }

        return BoundedHint(character.name)
            ?? BoundedHint(character.m_name)
            ?? BoundedHint(character.GetHoverName());
    }

    private static string? BoundedHint(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }

        var normalized = value!.Trim();
        if (normalized.EndsWith("(Clone)", StringComparison.Ordinal))
        {
            normalized = normalized.Substring(
                0,
                normalized.Length - "(Clone)".Length).TrimEnd();
        }
        if (normalized.StartsWith("$", StringComparison.Ordinal))
        {
            normalized = normalized.Substring(1).Replace('_', ' ');
        }

        return normalized.Length <= CompanionProtocol.MaximumCodeCharacters
            ? normalized
            : normalized.Substring(0, CompanionProtocol.MaximumCodeCharacters);
    }

    private static bool TryPosition(
        UnityEngine.Vector3 value,
        out CompanionPosition position)
    {
        position = new CompanionPosition(value.x, value.y, value.z);
        return IsCoordinate(value.x)
            && IsCoordinate(value.y)
            && IsCoordinate(value.z);
    }

    private static bool IsCoordinate(float value) =>
        !float.IsNaN(value)
        && !float.IsInfinity(value)
        && value >= -1_000_000f
        && value <= 1_000_000f;
#endif

    private void Trim(TimeSpan now)
    {
        while (recentOrder.Count > 0)
        {
            var oldest = recentOrder.Peek();
            if (now < SaturatingAdd(oldest.ObservedAt, DuplicateWindow))
            {
                return;
            }

            recentOrder.Dequeue();
            if (recentEvents.TryGetValue(oldest.Key, out var current)
                && current == oldest.ObservedAt)
            {
                recentEvents.Remove(oldest.Key);
            }
        }
    }

    private void TrimToCapacity()
    {
        while (recentOrder.Count > MaximumRecentEvents)
        {
            var oldest = recentOrder.Dequeue();
            if (recentEvents.TryGetValue(oldest.Key, out var current)
                && current == oldest.ObservedAt)
            {
                recentEvents.Remove(oldest.Key);
            }
        }
    }

    private static TimeSpan SaturatingAdd(TimeSpan value, TimeSpan duration) =>
        value > TimeSpan.MaxValue - duration
            ? TimeSpan.MaxValue
            : value + duration;

    private sealed class RecentEvent
    {
        public RecentEvent(string key, TimeSpan observedAt)
        {
            Key = key;
            ObservedAt = observedAt;
        }

        public string Key { get; }

        public TimeSpan ObservedAt { get; }
    }
}
