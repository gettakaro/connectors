using System.Text.Json;

namespace Takaro.Valheim.Companion.Protocol;

public static class CompanionProtocol
{
    // The RPC name is the transport channel both roles register, not a version marker.
    // It must stay stable forever: renaming it would make old and new companions register
    // different channels and exchange nothing at all, replacing a clear version-mismatch
    // message with silence. Version negotiation lives inside the envelope instead.
    public const string RpcName = "TakaroCompanionV1";
    public const int CurrentVersion = 2;

    // Raised to 2 alongside CurrentVersion. A protocol-1 companion rejects the ItemGrant
    // capability bit as unknown, so it would fail to parse a version-2 hello, answer
    // nothing, and be kicked in required mode with no diagnostic. Refusing to negotiate
    // produces an actionable "update the companion" message instead.
    public const int MinimumVersion = 2;
    public const int MaximumEnvelopeUtf8Bytes = 64 * 1024;
    public const int MaximumChatCharacters = 512;
    public const int MaximumInventoryStacks = 256;
    public const int MaximumCodeCharacters = 128;
    public const int MaximumEventIdCharacters = 64;
    public const int MaximumProductVersionCharacters = 128;
    public const int MaximumInventoryAmount = 1_000_000;
    public const int MaximumItemQuality = 1_000_000;
    public const int MaximumInventorySlot = MaximumInventoryStacks - 1;
    public const float MaximumDurability = 1_000_000_000f;
}

[Flags]
public enum CompanionCapability
{
    None = 0,
    Chat = 1,
    Inventory = 2,
    PlayerDeath = 4,
    EntityKilled = 8,
    ServerChat = 16,
    ItemGrant = 32
}

public sealed record CompanionEnvelope(
    int ProtocolVersion,
    string SessionNonce,
    long Sequence,
    string MessageId,
    string Type,
    JsonElement Payload);
