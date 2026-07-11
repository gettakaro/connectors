using System.Text.Json;

namespace Takaro.Valheim.Companion.Protocol;

public static class CompanionProtocol
{
    public const string RpcName = "TakaroCompanionV1";
    public const int CurrentVersion = 1;
    public const int MinimumVersion = 1;
    public const int MaximumEnvelopeUtf8Bytes = 64 * 1024;
    public const int MaximumChatCharacters = 512;
    public const int MaximumInventoryStacks = 256;
    public const int MaximumCodeCharacters = 128;
    public const int MaximumEventIdCharacters = 64;
}

[Flags]
public enum CompanionCapability
{
    None = 0,
    Chat = 1,
    Inventory = 2,
    PlayerDeath = 4,
    EntityKilled = 8
}

public sealed record CompanionEnvelope(
    int ProtocolVersion,
    string SessionNonce,
    long Sequence,
    string MessageId,
    string Type,
    JsonElement Payload);
