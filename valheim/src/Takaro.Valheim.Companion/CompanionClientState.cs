using System.Text.Json;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Companion;

public sealed class PreparedCompanionHelloAck
{
    internal PreparedCompanionHelloAck(
        long generation,
        CompanionEnvelope envelope)
    {
        Generation = generation;
        Envelope = envelope;
    }

    public long Generation { get; }

    public CompanionEnvelope Envelope { get; }
}

public sealed class CompanionClientState
{
    public static readonly TimeSpan HeartbeatInterval = TimeSpan.FromSeconds(5);

    private const int MaximumRetiredNonces = 16;
    private const CompanionCapability KnownCapabilities =
        CompanionCapability.Chat
        | CompanionCapability.Inventory
        | CompanionCapability.PlayerDeath
        | CompanionCapability.EntityKilled;

    private static readonly JsonSerializerOptions WireJson = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false
    };

    private readonly int minimumProtocolVersion;
    private readonly int maximumProtocolVersion;
    private readonly CompanionCapability supportedCapabilities;
    private readonly Queue<string> retiredNonceOrder = new();
    private readonly HashSet<string> retiredNonces = new(StringComparer.Ordinal);
    private PreparedCompanionHelloAck? pendingHelloAck;
    private string? activeNonce;
    private int activeProtocolVersion;
    private CompanionCapability activeCapabilities;
    private long generation;
    private long nextSequence;
    private TimeSpan nextHeartbeatAt = TimeSpan.MaxValue;

    public CompanionClientState(
        int minimumProtocolVersion,
        int maximumProtocolVersion,
        CompanionCapability supportedCapabilities)
    {
        if (minimumProtocolVersion <= 0
            || maximumProtocolVersion < minimumProtocolVersion)
        {
            throw new ArgumentOutOfRangeException(nameof(minimumProtocolVersion));
        }
        if ((supportedCapabilities & ~KnownCapabilities) != CompanionCapability.None)
        {
            throw new ArgumentOutOfRangeException(nameof(supportedCapabilities));
        }

        this.minimumProtocolVersion = minimumProtocolVersion;
        this.maximumProtocolVersion = maximumProtocolVersion;
        this.supportedCapabilities = supportedCapabilities;
    }

    public bool CanReport { get; private set; }

    public bool HasSession => pendingHelloAck is not null || activeNonce is not null;

    public string? SessionNonce => activeNonce;

    public bool TryPrepareHelloAck(
        CompanionEnvelope helloEnvelope,
        string productVersion,
        out PreparedCompanionHelloAck? prepared)
    {
        prepared = null;
        if (helloEnvelope is null
            || helloEnvelope.Type != CompanionMessageTypes.Hello
            || helloEnvelope.Sequence != 1
            || !IsStrictlyValidEnvelope(helloEnvelope)
            || retiredNonces.Contains(helloEnvelope.SessionNonce)
            || string.Equals(
                activeNonce,
                helloEnvelope.SessionNonce,
                StringComparison.Ordinal)
            || (pendingHelloAck is not null
                && string.Equals(
                    pendingHelloAck.Envelope.SessionNonce,
                    helloEnvelope.SessionNonce,
                    StringComparison.Ordinal))
            || !CompanionEnvelopeCodec.TryDecodePayload<CompanionHello>(
                helloEnvelope,
                out var hello,
                out _)
            || hello is null
            || !CompanionVersionPolicy.TryNegotiate(
                minimumProtocolVersion,
                maximumProtocolVersion,
                hello.MinimumVersion,
                hello.MaximumVersion,
                out var selectedProtocolVersion))
        {
            return false;
        }

        Reset(retireCurrentSession: true);
        var acceptedCapabilities = hello.Capabilities & supportedCapabilities;
        if (!TryCreateEnvelope(
                selectedProtocolVersion,
                helloEnvelope.SessionNonce,
                sequence: 1,
                messageId: $"hello-ack-{generation}",
                CompanionMessageTypes.HelloAck,
                new CompanionHelloAck(
                    selectedProtocolVersion,
                    productVersion,
                    acceptedCapabilities),
                out var envelope)
            || envelope is null)
        {
            return false;
        }

        prepared = new PreparedCompanionHelloAck(generation, envelope);
        pendingHelloAck = prepared;
        return true;
    }

    public bool ConfirmHelloAckSent(
        PreparedCompanionHelloAck prepared,
        TimeSpan monotonicNow)
    {
        if (prepared is null
            || monotonicNow < TimeSpan.Zero
            || !ReferenceEquals(pendingHelloAck, prepared)
            || prepared.Generation != generation
            || !CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloAck>(
                prepared.Envelope,
                out var helloAck,
                out _)
            || helloAck is null)
        {
            return false;
        }

        activeNonce = prepared.Envelope.SessionNonce;
        activeProtocolVersion = prepared.Envelope.ProtocolVersion;
        activeCapabilities = helloAck.AcceptedCapabilities;
        nextSequence = 2;
        nextHeartbeatAt = SaturatingAdd(monotonicNow, HeartbeatInterval);
        pendingHelloAck = null;
        CanReport = true;
        return true;
    }

    public void CancelHelloAck(PreparedCompanionHelloAck prepared)
    {
        if (prepared is not null && ReferenceEquals(pendingHelloAck, prepared))
        {
            pendingHelloAck = null;
        }
    }

    public bool TryCreateHeartbeat(
        TimeSpan monotonicNow,
        DateTimeOffset utcNow,
        out CompanionEnvelope? envelope)
    {
        envelope = null;
        if (!CanReport
            || monotonicNow < TimeSpan.Zero
            || monotonicNow < nextHeartbeatAt
            || !TryCreateOutboundEnvelope(
                CompanionMessageTypes.Heartbeat,
                new CompanionHeartbeat(utcNow.ToUnixTimeMilliseconds()),
                out envelope))
        {
            return false;
        }

        nextHeartbeatAt = SaturatingAdd(monotonicNow, HeartbeatInterval);
        return true;
    }

    public bool TryCreateReport<TPayload>(
        string messageType,
        TPayload payload,
        out CompanionEnvelope? envelope)
    {
        envelope = null;
        if (messageType != CompanionMessageTypes.Chat
            && messageType != CompanionMessageTypes.InventorySnapshot
            && messageType != CompanionMessageTypes.PlayerDeath
            && messageType != CompanionMessageTypes.EntityKilled)
        {
            return false;
        }

        var requiredCapability = RequiredCapability(messageType);
        if ((activeCapabilities & requiredCapability) != requiredCapability)
        {
            return false;
        }

        return TryCreateOutboundEnvelope(messageType, payload, out envelope);
    }

    public void Reset() => Reset(retireCurrentSession: true);

    public void ResetConnection()
    {
        Reset(retireCurrentSession: false);
        retiredNonceOrder.Clear();
        retiredNonces.Clear();
    }

    private bool TryCreateOutboundEnvelope<TPayload>(
        string messageType,
        TPayload payload,
        out CompanionEnvelope? envelope)
    {
        envelope = null;
        if (!CanReport
            || activeNonce is null
            || nextSequence <= 0)
        {
            return false;
        }

        var sequence = nextSequence;
        if (!TryCreateEnvelope(
                activeProtocolVersion,
                activeNonce,
                sequence,
                $"client-{sequence}",
                messageType,
                payload,
                out envelope))
        {
            return false;
        }

        nextSequence = sequence == long.MaxValue ? 0 : sequence + 1;
        return true;
    }

    private void Reset(bool retireCurrentSession)
    {
        if (retireCurrentSession)
        {
            RetireNonce(activeNonce);
            RetireNonce(pendingHelloAck?.Envelope.SessionNonce);
        }

        pendingHelloAck = null;
        activeNonce = null;
        activeProtocolVersion = 0;
        activeCapabilities = CompanionCapability.None;
        nextSequence = 0;
        nextHeartbeatAt = TimeSpan.MaxValue;
        CanReport = false;
        generation = NextGeneration(generation);
    }

    private void RetireNonce(string? nonce)
    {
        if (string.IsNullOrEmpty(nonce) || !retiredNonces.Add(nonce!))
        {
            return;
        }

        retiredNonceOrder.Enqueue(nonce!);
        while (retiredNonceOrder.Count > MaximumRetiredNonces)
        {
            retiredNonces.Remove(retiredNonceOrder.Dequeue());
        }
    }

    private static bool TryCreateEnvelope<TPayload>(
        int protocolVersion,
        string nonce,
        long sequence,
        string messageId,
        string messageType,
        TPayload payload,
        out CompanionEnvelope? envelope)
    {
        envelope = null;
        try
        {
            var candidate = new CompanionEnvelope(
                protocolVersion,
                nonce,
                sequence,
                messageId,
                messageType,
                JsonSerializer.SerializeToElement(payload, WireJson));
            _ = CompanionEnvelopeCodec.EncodeEnvelope(candidate);
            envelope = candidate;
            return true;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (JsonException)
        {
            return false;
        }
        catch (NotSupportedException)
        {
            return false;
        }
    }

    private static bool IsStrictlyValidEnvelope(CompanionEnvelope envelope)
    {
        try
        {
            _ = CompanionEnvelopeCodec.EncodeEnvelope(envelope);
            return true;
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    private static CompanionCapability RequiredCapability(string messageType)
    {
        switch (messageType)
        {
            case CompanionMessageTypes.Chat:
                return CompanionCapability.Chat;
            case CompanionMessageTypes.InventorySnapshot:
                return CompanionCapability.Inventory;
            case CompanionMessageTypes.PlayerDeath:
                return CompanionCapability.PlayerDeath;
            case CompanionMessageTypes.EntityKilled:
                return CompanionCapability.EntityKilled;
            default:
                return CompanionCapability.None;
        }
    }

    private static long NextGeneration(long value) =>
        value == long.MaxValue ? 1 : value + 1;

    private static TimeSpan SaturatingAdd(TimeSpan value, TimeSpan duration) =>
        value > TimeSpan.MaxValue - duration
            ? TimeSpan.MaxValue
            : value + duration;
}
