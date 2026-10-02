using System.Text.Json;

namespace Takaro.Valheim.Companion.Protocol;

/// <summary>
/// Wire contract between the dedicated-server plugin and the optional client inventory
/// companion. Everything the companion sends is client-reported and untrusted: the server
/// binds it to the network peer it arrived on and never lets it claim an identity.
/// </summary>
public static class CompanionProtocol
{
    // The RPC name is the transport channel both roles register, not a version marker.
    // It must stay stable forever: renaming it would make old and new companions register
    // different channels and exchange nothing at all, replacing a clear version-mismatch
    // log line with silence. Version negotiation lives inside the envelope instead.
    public const string RpcName = "TakaroCompanionV1";

    // Protocol 3 is the inventory-only companion: inventory snapshots plus kill verdicts.
    // Protocol 2 companions (chat, deaths, item grants) are refused, never kicked.
    public const int CurrentVersion = 3;
    public const int MinimumVersion = 3;

    // The server's hello travels in a protocol-2 envelope with a protocol-2-readable payload,
    // so a protocol-2 companion can still read it and answer with a hello-nack. That nack is
    // what lets the server log "companion too old" instead of seeing nothing at all.
    public const int NegotiationEnvelopeVersion = 2;

    // Negotiation messages (hello, hello-ack, hello-nack) decode at any envelope version in
    // this range, so a mismatch is always reported instead of silently dropped.
    public const int MaximumNegotiableVersion = 1000;

    public const int MaximumEnvelopeUtf8Bytes = 64 * 1024;
    public const int MaximumNameCharacters = 512;
    public const int MaximumInventoryStacks = 256;
    public const int MaximumCodeCharacters = 128;
    public const int MaximumProductVersionCharacters = 128;
    public const int MaximumZdoIdCharacters = 64;
    public const int MaximumInventoryAmount = 1_000_000;
    public const int MaximumItemQuality = 1_000_000;
    public const int MaximumInventorySlot = MaximumInventoryStacks - 1;
    public const float MaximumDurability = 1_000_000_000f;
}

/// <summary>
/// Capability bits. Protocol 3 knows only <see cref="Inventory"/>; the kill verdict is part
/// of protocol 3 itself. A hello from an older server may carry legacy bits, which are
/// tolerated in the hello only so the companion can answer it with a clear nack.
/// </summary>
[Flags]
public enum CompanionCapability
{
    None = 0,
    Inventory = 2
}

public static class CompanionAttackerKind
{
    public const string LocalPlayer = "local-player";
    public const string OtherPlayer = "other-player";
    public const string Creature = "creature";
    public const string None = "none";

    public static bool IsKnown(string? value) =>
        value == LocalPlayer
        || value == OtherPlayer
        || value == Creature
        || value == None;
}

public sealed record CompanionEnvelope(
    int ProtocolVersion,
    string SessionNonce,
    long Sequence,
    string MessageId,
    string Type,
    JsonElement Payload);
