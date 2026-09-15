using System.Globalization;
#if TAKARO_VALHEIM_COMPANION
using UnityEngine;
#endif

namespace Takaro.Valheim.Companion;

/// <summary>
/// Outcome of applying one item grant to the local player's inventory.
/// <paramref name="Delivered"/> and <paramref name="Dropped"/> always sum to the granted
/// amount when the item resolved, so nothing a grant contained is ever unaccounted for.
/// </summary>
public sealed class CompanionItemGrantOutcome
{
    public CompanionItemGrantOutcome(bool resolved, int requested, int delivered, int dropped)
    {
        Resolved = resolved;
        Requested = requested;
        Delivered = delivered;
        Dropped = dropped;
    }

    /// <summary>False when the item code or player state could not be resolved, in which
    /// case nothing was added and nothing was dropped.</summary>
    public bool Resolved { get; }

    public int Requested { get; }

    public int Delivered { get; }

    public int Dropped { get; }

    public static CompanionItemGrantOutcome Unresolved(int requested) =>
        new(false, requested, 0, 0);
}

/// <summary>
/// Pure, game-independent half of the item-grant apply path. Kept separate from the Unity
/// shim below so the accounting can be unit tested without Valheim assemblies, matching how
/// <see cref="CompanionInventoryReader"/> splits its logic.
/// </summary>
public static class CompanionItemGrantMath
{
    /// <summary>
    /// Derives the outcome from an observed inventory count difference.
    /// Valheim's <c>Inventory.AddItem</c> fills partially and keeps whatever it managed to
    /// deposit, so its return value cannot be trusted to mean "nothing happened". Counting
    /// the item before and after is exact regardless of how far the insert got.
    /// </summary>
    public static CompanionItemGrantOutcome FromCountDelta(int requested, int countBefore, int countAfter)
    {
        if (requested <= 0)
        {
            return new CompanionItemGrantOutcome(true, requested, 0, 0);
        }

        var delta = countAfter - countBefore;

        // Clamp defensively: a negative or oversized delta means something else mutated the
        // inventory between the two reads, and inventing a number there would either destroy
        // items or duplicate them. Treating the surplus as delivered is the safe direction.
        var delivered = delta < 0 ? 0 : delta > requested ? requested : delta;
        return new CompanionItemGrantOutcome(true, requested, delivered, requested - delivered);
    }

    public static string DescribeOutcome(CompanionItemGrantOutcome outcome, string itemName)
    {
        if (!outcome.Resolved)
        {
            return string.Empty;
        }

        if (outcome.Dropped <= 0)
        {
            return string.Format(
                CultureInfo.InvariantCulture,
                "Received {0}x {1}.",
                outcome.Delivered,
                itemName);
        }

        if (outcome.Delivered <= 0)
        {
            return string.Format(
                CultureInfo.InvariantCulture,
                "Your inventory was full - {0}x {1} dropped at your feet.",
                outcome.Dropped,
                itemName);
        }

        return string.Format(
            CultureInfo.InvariantCulture,
            "Received {0}x {1}; inventory full, {2} dropped at your feet.",
            outcome.Delivered,
            itemName,
            outcome.Dropped);
    }
}

#if TAKARO_VALHEIM_COMPANION
public static class CompanionInventoryWriter
{
    /// <summary>
    /// Places a granted stack in the local player's inventory, dropping any shortfall in the
    /// world at the player's feet so a full inventory can never swallow the grant.
    /// Must run on Unity's main thread; the inbound RPC handler already does.
    /// </summary>
    public static CompanionItemGrantOutcome Apply(string code, int amount, int quality, out string itemName)
    {
        itemName = code;
        if (string.IsNullOrWhiteSpace(code) || amount <= 0)
        {
            return CompanionItemGrantOutcome.Unresolved(amount);
        }

        var player = Player.m_localPlayer;
        // Unity overloads op_Equality to report destroyed-but-not-collected objects as null,
        // so this must stay '== null' rather than 'is null'.
        if (player == null || ObjectDB.instance == null)
        {
            return CompanionItemGrantOutcome.Unresolved(amount);
        }

        var inventory = player.GetInventory();
        if (inventory is null)
        {
            return CompanionItemGrantOutcome.Unresolved(amount);
        }

        var prefab = ObjectDB.instance.GetItemPrefab(code);
        if (prefab == null || !prefab.TryGetComponent<ItemDrop>(out var itemDrop))
        {
            return CompanionItemGrantOutcome.Unresolved(amount);
        }

        var sharedName = itemDrop.m_itemData?.m_shared?.m_name;
        if (string.IsNullOrEmpty(sharedName))
        {
            return CompanionItemGrantOutcome.Unresolved(amount);
        }

        itemName = ResolveDisplayName(sharedName!, code);

        var before = inventory.CountItems(sharedName, quality, true);
        inventory.AddItem(code, amount, quality, 0, 0L, string.Empty, true);
        var after = inventory.CountItems(sharedName, quality, true);

        var outcome = CompanionItemGrantMath.FromCountDelta(amount, before, after);
        if (outcome.Dropped > 0)
        {
            DropShortfall(prefab, itemDrop, player, outcome.Dropped, quality);
        }

        return outcome;
    }

    private static void DropShortfall(GameObject prefab, ItemDrop itemDrop, Player player, int amount, int quality)
    {
        // Mirrors the connector's server-side drop so a shortfall lands exactly like an
        // ordinary world drop, using Valheim's own replicated spawn path.
        var position = player.transform.position + (player.transform.forward * 0.6f) + (Vector3.up * 0.4f);
        var itemData = itemDrop.m_itemData.Clone();
        itemData.m_dropPrefab = prefab;
        itemData.m_quality = quality;
        itemData.m_stack = amount;
        ItemDrop.DropItem(itemData, amount, position, Quaternion.identity);
    }

    private static string ResolveDisplayName(string sharedName, string fallback)
    {
        var trimmed = sharedName.Trim().Trim('$');
        return string.IsNullOrWhiteSpace(trimmed) ? fallback : trimmed;
    }
}
#endif
