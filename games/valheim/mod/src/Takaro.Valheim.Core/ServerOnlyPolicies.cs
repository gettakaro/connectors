namespace Takaro.Valheim.Core;

public static class ValheimRuntimePolicy
{
    private static readonly HashSet<string> DedicatedServerExecutableNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "valheim_server",
        "valheim_server.exe",
        "valheim_server.x86_64",
        "valheim_server.x86_64.exe"
    };

    public static bool IsDedicatedServerProcess(
        bool isBatchMode,
        string? processName,
        string? executablePath) =>
        isBatchMode
        && (IsDedicatedServerExecutable(processName) || IsDedicatedServerExecutable(executablePath));

    /// <summary>The value of the dedicated server's <c>-name</c> launch argument, if any.</summary>
    public static string? ServerNameArgument(IReadOnlyList<string>? args)
    {
        if (args is null)
        {
            return null;
        }

        for (var i = 0; i < args.Count - 1; i++)
        {
            if (string.Equals(args[i], "-name", StringComparison.OrdinalIgnoreCase)
                && !string.IsNullOrWhiteSpace(args[i + 1]))
            {
                return args[i + 1].Trim();
            }
        }

        return null;
    }

    private static bool IsDedicatedServerExecutable(string? identity)
    {
        if (string.IsNullOrWhiteSpace(identity))
        {
            return false;
        }

        var normalized = identity!.Trim().Replace('\\', '/');
        var executableName = normalized.Substring(normalized.LastIndexOf('/') + 1);
        return DedicatedServerExecutableNames.Contains(executableName);
    }
}

public sealed record GiveItemStackPlan(
    bool Success,
    IReadOnlyList<int> Stacks,
    string? ErrorCode,
    string? ErrorMessage);

public static class GiveItemPolicy
{
    public const int MaxAmount = 1000;
    public const int MaxDropStacks = 100;

    public static GiveItemStackPlan PlanStacks(int amount, int maxStackSize)
    {
        if (amount < 1 || amount > MaxAmount)
        {
            return Error(
                "invalid_amount",
                $"Valheim item amount must be between 1 and {MaxAmount}, got {amount}.");
        }

        var effectiveStackSize = maxStackSize > 0 ? maxStackSize : amount;
        var stackCount = (amount + (long)effectiveStackSize - 1) / effectiveStackSize;
        if (stackCount > MaxDropStacks)
        {
            return Error(
                "item_drop_limit_exceeded",
                $"Valheim giveItem would create {stackCount} world drops; the limit is {MaxDropStacks} per request.");
        }

        var stacks = new List<int>((int)stackCount);
        var remaining = amount;
        while (remaining > 0)
        {
            var stack = Math.Min(remaining, effectiveStackSize);
            stacks.Add(stack);
            remaining -= stack;
        }

        return new GiveItemStackPlan(true, stacks, null, null);
    }

    private static GiveItemStackPlan Error(string code, string message) =>
        new(false, Array.Empty<int>(), code, message);
}

public static class RuntimeArrayActionPolicy
{
    public static TakaroActionResult FromSource<T>(
        bool sourceAvailable,
        IEnumerable<T>? values,
        string sourceName)
    {
        if (!sourceAvailable || values is null)
        {
            return TakaroActionResult.Error(
                "runtime_unavailable",
                $"{sourceName} is not available yet; the connector cannot confirm an empty result.");
        }

        return TakaroActionResult.Ok(values.ToArray());
    }
}

public static class RuntimePlayerActionPolicy
{
    public static TakaroActionResult Find(TakaroActionResult playersResult, string identifier)
    {
        if (!playersResult.Success)
        {
            return playersResult;
        }

        if (playersResult.Payload is not IEnumerable<TakaroPlayer> players)
        {
            return TakaroActionResult.Error(
                "runtime_unavailable",
                "Valheim player list returned an unavailable runtime payload.");
        }

        var player = PlayerMapper.Find(players, identifier);
        return player is null
            ? TakaroActionResult.Error("player_not_found", $"Valheim player '{identifier}' is not online.")
            : TakaroActionResult.Ok(player);
    }
}

public enum ValheimEventObservationSource
{
    Connector,
    ServerPlayerSnapshot,
    PeerBoundRoutedRpc,
    GameKillReport,
    UnboundRoutedRpc
}

public static class ValheimEventType
{
    public const string Log = "log";
    public const string PlayerConnected = "player-connected";
    public const string PlayerDisconnected = "player-disconnected";
    public const string ChatMessage = "chat-message";
    public const string PlayerDeath = "player-death";
    public const string EntityKilled = "entity-killed";
}

/// <summary>
/// Explicit allowlist of which server-side observation may produce which Takaro event. Every
/// event is server-observed: identity always comes from the network peer a packet arrived on,
/// never from a value the client wrote into the packet.
/// </summary>
public static class ValheimEventAcceptancePolicy
{
    public static bool CanEmit(string eventType, ValheimEventObservationSource source) =>
        (eventType, source) switch
        {
            (ValheimEventType.Log, ValheimEventObservationSource.Connector) => true,
            (ValheimEventType.PlayerConnected, ValheimEventObservationSource.ServerPlayerSnapshot) => true,
            (ValheimEventType.PlayerDisconnected, ValheimEventObservationSource.ServerPlayerSnapshot) => true,
            (ValheimEventType.ChatMessage, ValheimEventObservationSource.PeerBoundRoutedRpc) => true,
            (ValheimEventType.PlayerDeath, ValheimEventObservationSource.PeerBoundRoutedRpc) => true,
            (ValheimEventType.EntityKilled, ValheimEventObservationSource.GameKillReport) => true,
            _ => false
        };
}
