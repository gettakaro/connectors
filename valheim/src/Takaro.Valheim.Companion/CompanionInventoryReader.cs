using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Companion;

public sealed class CompanionInventorySourceItem
{
    public CompanionInventorySourceItem(
        string? code,
        string? name,
        int amount,
        int quality,
        float durability,
        bool equipped,
        int slot)
    {
        Code = code;
        Name = name;
        Amount = amount;
        Quality = quality;
        Durability = durability;
        Equipped = equipped;
        Slot = slot;
    }

    public string? Code { get; }

    public string? Name { get; }

    public int Amount { get; }

    public int Quality { get; }

    public float Durability { get; }

    public bool Equipped { get; }

    public int Slot { get; }
}

public sealed class CompanionInventorySnapshot
{
    internal CompanionInventorySnapshot(
        IReadOnlyList<CompanionInventoryStack> stacks,
        string fingerprint)
    {
        Stacks = stacks;
        Fingerprint = fingerprint;
    }

    public IReadOnlyList<CompanionInventoryStack> Stacks { get; }

    public string Fingerprint { get; }
}

public sealed class CompanionInventoryReader
{
    private string? lastSentFingerprint;

    public bool TryGetChanged(
        IReadOnlyList<CompanionInventorySourceItem>? items,
        out CompanionInventorySnapshot? snapshot)
    {
        snapshot = null;
        if (items is null)
        {
            return false;
        }

        var stacks = items
            .Select(Normalize)
            .Where(stack => stack is not null)
            .Cast<CompanionInventoryStack>()
            .OrderBy(stack => stack.Slot)
            .ThenBy(stack => stack.Code, StringComparer.Ordinal)
            .ThenBy(stack => stack.Name, StringComparer.Ordinal)
            .ThenBy(stack => stack.Quality)
            .ThenBy(stack => stack.Amount)
            .ThenBy(stack => stack.Durability)
            .ThenBy(stack => stack.Equipped)
            .Take(CompanionProtocol.MaximumInventoryStacks)
            .ToArray();
        var fingerprint = Fingerprint(stacks);
        if (string.Equals(
            fingerprint,
            lastSentFingerprint,
            StringComparison.Ordinal))
        {
            return false;
        }

        snapshot = new CompanionInventorySnapshot(stacks, fingerprint);
        return true;
    }

    public void MarkSent(CompanionInventorySnapshot snapshot)
    {
        if (snapshot is null)
        {
            throw new ArgumentNullException(nameof(snapshot));
        }

        lastSentFingerprint = snapshot.Fingerprint;
    }

    public void Reset()
    {
        lastSentFingerprint = null;
    }

#if TAKARO_VALHEIM_COMPANION
    internal bool TryReadChanged(
        Player? player,
        out CompanionInventorySnapshot? snapshot)
    {
        snapshot = null;
        if (player == null)
        {
            return false;
        }

        var inventory = player.GetInventory();
        if (inventory is null)
        {
            return false;
        }

        var width = Math.Max(1, inventory.GetWidth());
        var sourceItems = inventory.GetAllItems()
            .Select(item =>
            {
                var rawName = item.m_shared?.m_name;
                var code = item.m_dropPrefab != null
                    ? item.m_dropPrefab.name
                    : rawName;
                var readableName = string.IsNullOrWhiteSpace(rawName)
                    || rawName!.StartsWith("$", StringComparison.Ordinal)
                        ? code
                        : rawName;
                var slotValue = ((long)item.m_gridPos.y * width) + item.m_gridPos.x;
                var slot = slotValue < int.MinValue || slotValue > int.MaxValue
                    ? -1
                    : (int)slotValue;
                return new CompanionInventorySourceItem(
                    code,
                    readableName,
                    item.m_stack,
                    item.m_quality,
                    item.m_durability,
                    item.m_equipped,
                    slot);
            })
            .ToArray();

        return TryGetChanged(sourceItems, out snapshot);
    }
#endif

    private static CompanionInventoryStack? Normalize(
        CompanionInventorySourceItem item)
    {
        var code = BoundedRequired(
            item.Code,
            CompanionProtocol.MaximumCodeCharacters);
        if (code is null
            || item.Amount <= 0
            || item.Slot < 0
            || item.Slot > CompanionProtocol.MaximumInventorySlot)
        {
            return null;
        }

        var name = BoundedRequired(
            item.Name,
            CompanionProtocol.MaximumChatCharacters) ?? code;
        var durability = float.IsNaN(item.Durability)
            || float.IsInfinity(item.Durability)
                ? 0
                : Math.Max(
                    0,
                    Math.Min(
                        item.Durability,
                        CompanionProtocol.MaximumDurability));
        return new CompanionInventoryStack(
            code,
            name,
            Math.Min(item.Amount, CompanionProtocol.MaximumInventoryAmount),
            Math.Max(
                1,
                Math.Min(
                    item.Quality,
                    CompanionProtocol.MaximumItemQuality)),
            durability,
            item.Equipped,
            item.Slot);
    }

    private static string? BoundedRequired(string? value, int maximumCharacters)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }

        var trimmed = value!.Trim();
        return trimmed.Length <= maximumCharacters
            ? trimmed
            : trimmed.Substring(0, maximumCharacters);
    }

    private static string Fingerprint(
        IReadOnlyList<CompanionInventoryStack> stacks)
    {
        var canonical = new StringBuilder();
        foreach (var stack in stacks)
        {
            Append(canonical, stack.Code);
            Append(canonical, stack.Name);
            canonical.Append(stack.Amount).Append('|');
            canonical.Append(stack.Quality).Append('|');
            canonical.Append(stack.Durability.ToString("R", CultureInfo.InvariantCulture)).Append('|');
            canonical.Append(stack.Equipped ? '1' : '0').Append('|');
            canonical.Append(stack.Slot).Append(';');
        }

        using var sha256 = SHA256.Create();
        var hash = sha256.ComputeHash(Encoding.UTF8.GetBytes(canonical.ToString()));
        return BitConverter.ToString(hash).Replace("-", string.Empty);
    }

    private static void Append(StringBuilder target, string value)
    {
        target.Append(value.Length).Append(':').Append(value).Append('|');
    }
}
