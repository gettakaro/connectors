namespace Takaro.Valheim.Companion.Protocol;

public static class CompanionMessageTypes
{
    public const string Hello = "hello";
    public const string HelloAck = "hello-ack";
    public const string HelloNack = "hello-nack";
    public const string Heartbeat = "heartbeat";
    public const string Chat = "chat";
    public const string ServerChat = "server-chat";
    public const string InventorySnapshot = "inventory-snapshot";
    public const string PlayerDeath = "player-death";
    public const string EntityKilled = "entity-killed";
    public const string ItemGrant = "item-grant";
}

public sealed record CompanionHello(
    int MinimumVersion,
    int MaximumVersion,
    CompanionCapability Capabilities);

public sealed record CompanionHelloAck(
    int ProtocolVersion,
    string ProductVersion,
    CompanionCapability AcceptedCapabilities);

public sealed record CompanionHelloNack(
    int MinimumVersion,
    int MaximumVersion,
    string ProductVersion);

public sealed record CompanionHeartbeat(long TimestampUnixMilliseconds);

public sealed record CompanionChatReport(
    string EventId,
    long TimestampUnixMilliseconds,
    string Message);

public sealed record CompanionServerChatMessage(
    string Sender,
    string Message);

/// <summary>
/// Server-to-client instruction to place items in the local player's inventory.
/// Like every other server-to-client message it carries no event id and no timestamp:
/// the server has already answered Takaro by the time this is sent, and the companion
/// reports the real outcome through its ordinary inventory snapshot.
/// </summary>
public sealed record CompanionItemGrant(
    string Code,
    int Amount,
    int Quality);

public sealed record CompanionInventoryReport(IReadOnlyList<CompanionInventoryStack> Stacks);

public sealed record CompanionInventoryStack(
    string Code,
    string Name,
    int Amount,
    int Quality,
    float Durability,
    bool Equipped,
    int Slot);

public sealed record CompanionPosition(float X, float Y, float Z);

public sealed record CompanionPlayerDeathReport(
    string EventId,
    long TimestampUnixMilliseconds,
    CompanionPosition Position,
    string? CauseHint,
    string? AttackerCodeHint);

public sealed record CompanionEntityKilledReport(
    string EventId,
    long TimestampUnixMilliseconds,
    CompanionPosition Position,
    string? EntityCodeHint,
    string? WeaponCodeHint);
