namespace Takaro.Valheim.Companion.Protocol;

public static class CompanionMessageTypes
{
    public const string Hello = "hello";
    public const string HelloAck = "hello-ack";
    public const string HelloNack = "hello-nack";
    public const string Heartbeat = "heartbeat";
    public const string InventorySnapshot = "inventory-snapshot";
    public const string KillVerdict = "kill-verdict";

    public static bool IsNegotiation(string? type) =>
        type == Hello || type == HelloAck || type == HelloNack;
}

/// <summary>Server to client: the protocol range the server speaks.</summary>
public sealed record CompanionHello(
    int MinimumVersion,
    int MaximumVersion,
    int Capabilities);

/// <summary>Client to server: the protocol the companion selected.</summary>
public sealed record CompanionHelloAck(
    int ProtocolVersion,
    string ProductVersion,
    CompanionCapability AcceptedCapabilities);

/// <summary>Either side: no common protocol; carries the sender's own range.</summary>
public sealed record CompanionHelloNack(
    int MinimumVersion,
    int MaximumVersion,
    string ProductVersion);

public sealed record CompanionHeartbeat(long TimestampUnixMilliseconds);

public sealed record CompanionInventoryReport(IReadOnlyList<CompanionInventoryStack> Stacks);

public sealed record CompanionInventoryStack(
    string Code,
    string Name,
    int Amount,
    int Quality,
    float Durability,
    bool Equipped,
    int Slot);

/// <summary>
/// Client to server, sent once when a non-player creature owned by this client dies and the
/// local player's name is marked on it as an attacker. A supplement to the server's own kill
/// detection, never a source of kills on its own.
/// </summary>
public sealed record CompanionKillVerdict(
    string CreatureZdo,
    string Prefab,
    string EnemyToken,
    bool LastHitByLocalPlayer,
    string LastHitAttackerKind);
