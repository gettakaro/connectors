namespace Takaro.Valheim.Companion.Protocol;

public static class CompanionMessageTypes
{
    public const string Hello = "hello";
    public const string HelloAck = "hello-ack";
    public const string Heartbeat = "heartbeat";
    public const string Chat = "chat";
    public const string InventorySnapshot = "inventory-snapshot";
    public const string PlayerDeath = "player-death";
    public const string EntityKilled = "entity-killed";
}

public sealed record CompanionHello(
    int MinimumVersion,
    int MaximumVersion,
    CompanionCapability Capabilities);

public sealed record CompanionHelloAck(
    int ProtocolVersion,
    string ProductVersion,
    CompanionCapability AcceptedCapabilities);

public sealed record CompanionHeartbeat(long TimestampUnixMilliseconds);

public sealed record CompanionChatReport(
    string EventId,
    long TimestampUnixMilliseconds,
    string Message);

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
