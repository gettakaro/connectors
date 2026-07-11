using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

public enum CompanionSessionDecision
{
    Accept,
    RejectUnknownPeer,
    RejectNotNegotiated,
    RejectNonce,
    RejectSequence,
    RejectVersion,
    Expired,
    RejectMetadata
}

public sealed record CompanionSessionBegin(
    long PeerId,
    string Nonce,
    DateTimeOffset HandshakeDeadline);

public sealed record CompanionSessionSnapshot(
    long PeerId,
    string Nonce,
    DateTimeOffset HandshakeDeadline,
    bool IsNegotiated,
    int? SelectedProtocolVersion,
    string? ProductVersion,
    CompanionCapability Capabilities,
    long LastSequence,
    DateTimeOffset? LastHeartbeat,
    DateTimeOffset ExpiresAt);

public sealed class CompanionSessionRegistry
{
    public const int MaximumProductVersionCharacters = 128;

    private const CompanionCapability KnownCapabilities =
        CompanionCapability.Chat
        | CompanionCapability.Inventory
        | CompanionCapability.PlayerDeath
        | CompanionCapability.EntityKilled;

    private readonly int minimumProtocolVersion;
    private readonly int maximumProtocolVersion;
    private readonly CompanionCapability supportedCapabilities;
    private readonly TimeSpan handshakeGrace;
    private readonly TimeSpan heartbeatGrace;
    private readonly Dictionary<long, Session> sessions = new();
    private readonly object syncRoot = new();
    private object? currentWorldIdentity;
    private bool hasCurrentWorldIdentity;

    public CompanionSessionRegistry(
        int minimumProtocolVersion,
        int maximumProtocolVersion,
        CompanionCapability supportedCapabilities,
        TimeSpan handshakeGrace,
        TimeSpan heartbeatGrace)
    {
        if (minimumProtocolVersion <= 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(minimumProtocolVersion),
                "Minimum protocol version must be positive.");
        }
        if (maximumProtocolVersion < minimumProtocolVersion)
        {
            throw new ArgumentOutOfRangeException(
                nameof(maximumProtocolVersion),
                "Maximum protocol version must not be lower than the minimum.");
        }
        if (!HasOnlyKnownCapabilities(supportedCapabilities))
        {
            throw new ArgumentOutOfRangeException(
                nameof(supportedCapabilities),
                "Supported capabilities contain unknown flags.");
        }
        if (handshakeGrace <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(
                nameof(handshakeGrace),
                "Handshake grace must be positive.");
        }
        if (heartbeatGrace <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(
                nameof(heartbeatGrace),
                "Heartbeat grace must be positive.");
        }

        this.minimumProtocolVersion = minimumProtocolVersion;
        this.maximumProtocolVersion = maximumProtocolVersion;
        this.supportedCapabilities = supportedCapabilities;
        this.handshakeGrace = handshakeGrace;
        this.heartbeatGrace = heartbeatGrace;
    }

    public CompanionSessionBegin Begin(long peerId, DateTimeOffset now, string nonce)
    {
        if (string.IsNullOrWhiteSpace(nonce))
        {
            throw new ArgumentException("Session nonce must not be blank.", nameof(nonce));
        }

        var deadline = now + handshakeGrace;
        lock (syncRoot)
        {
            sessions[peerId] = new Session(peerId, nonce, deadline);
        }

        return new CompanionSessionBegin(peerId, nonce, deadline);
    }

    public CompanionSessionDecision CompleteHelloAck(
        long peerId,
        string nonce,
        int selectedProtocolVersion,
        string productVersion,
        CompanionCapability capabilities,
        long sequence,
        DateTimeOffset now)
    {
        lock (syncRoot)
        {
            if (!sessions.TryGetValue(peerId, out var session))
            {
                return CompanionSessionDecision.RejectUnknownPeer;
            }
            if (!NonceMatches(session, nonce))
            {
                return CompanionSessionDecision.RejectNonce;
            }
            if (IsExpired(session, now))
            {
                return CompanionSessionDecision.Expired;
            }
            if (session.IsNegotiated)
            {
                return CompanionSessionDecision.RejectNotNegotiated;
            }
            if (selectedProtocolVersion < minimumProtocolVersion
                || selectedProtocolVersion > maximumProtocolVersion)
            {
                return CompanionSessionDecision.RejectVersion;
            }
            if (!IsValidProductVersion(productVersion)
                || !HasOnlyKnownCapabilities(capabilities)
                || (capabilities & ~supportedCapabilities) != CompanionCapability.None)
            {
                return CompanionSessionDecision.RejectMetadata;
            }
            if (sequence <= session.LastSequence || sequence <= 0)
            {
                return CompanionSessionDecision.RejectSequence;
            }

            session.IsNegotiated = true;
            session.SelectedProtocolVersion = selectedProtocolVersion;
            session.ProductVersion = productVersion.Trim();
            session.Capabilities = capabilities;
            session.LastSequence = sequence;
            session.LastHeartbeat = now;
            return CompanionSessionDecision.Accept;
        }
    }

    public CompanionSessionDecision ValidateReport(
        long peerId,
        string nonce,
        int protocolVersion,
        long sequence,
        DateTimeOffset now)
    {
        lock (syncRoot)
        {
            return ValidateNegotiatedEnvelopeLocked(
                peerId,
                nonce,
                protocolVersion,
                sequence,
                now,
                refreshHeartbeat: false);
        }
    }

    public CompanionSessionDecision ValidateHeartbeat(
        long peerId,
        string nonce,
        int protocolVersion,
        long sequence,
        DateTimeOffset now)
    {
        lock (syncRoot)
        {
            return ValidateNegotiatedEnvelopeLocked(
                peerId,
                nonce,
                protocolVersion,
                sequence,
                now,
                refreshHeartbeat: true);
        }
    }

    public bool TryGetSnapshot(long peerId, out CompanionSessionSnapshot snapshot)
    {
        lock (syncRoot)
        {
            if (!sessions.TryGetValue(peerId, out var session))
            {
                snapshot = default!;
                return false;
            }

            snapshot = CreateSnapshot(session);
            return true;
        }
    }

    public void RemovePeer(long peerId)
    {
        lock (syncRoot)
        {
            sessions.Remove(peerId);
        }
    }

    public void SwitchWorld(object? worldIdentity)
    {
        lock (syncRoot)
        {
            if (hasCurrentWorldIdentity && Equals(currentWorldIdentity, worldIdentity))
            {
                return;
            }

            sessions.Clear();
            currentWorldIdentity = worldIdentity;
            hasCurrentWorldIdentity = true;
        }
    }

    private CompanionSessionDecision ValidateNegotiatedEnvelopeLocked(
        long peerId,
        string nonce,
        int protocolVersion,
        long sequence,
        DateTimeOffset now,
        bool refreshHeartbeat)
    {
        if (!sessions.TryGetValue(peerId, out var session))
        {
            return CompanionSessionDecision.RejectUnknownPeer;
        }
        if (!NonceMatches(session, nonce))
        {
            return CompanionSessionDecision.RejectNonce;
        }
        if (IsExpired(session, now))
        {
            return CompanionSessionDecision.Expired;
        }
        if (!session.IsNegotiated)
        {
            return CompanionSessionDecision.RejectNotNegotiated;
        }
        if (protocolVersion != session.SelectedProtocolVersion)
        {
            return CompanionSessionDecision.RejectVersion;
        }
        if (sequence <= session.LastSequence || sequence <= 0)
        {
            return CompanionSessionDecision.RejectSequence;
        }

        session.LastSequence = sequence;
        if (refreshHeartbeat)
        {
            session.LastHeartbeat = now;
        }

        return CompanionSessionDecision.Accept;
    }

    private bool IsExpired(Session session, DateTimeOffset now)
    {
        if (!session.IsNegotiated)
        {
            return now >= session.HandshakeDeadline;
        }

        return now >= session.LastHeartbeat!.Value + heartbeatGrace;
    }

    private CompanionSessionSnapshot CreateSnapshot(Session session)
    {
        var expiresAt = session.IsNegotiated
            ? session.LastHeartbeat!.Value + heartbeatGrace
            : session.HandshakeDeadline;
        return new CompanionSessionSnapshot(
            session.PeerId,
            session.Nonce,
            session.HandshakeDeadline,
            session.IsNegotiated,
            session.SelectedProtocolVersion,
            session.ProductVersion,
            session.Capabilities,
            session.LastSequence,
            session.LastHeartbeat,
            expiresAt);
    }

    private static bool NonceMatches(Session session, string nonce) =>
        string.Equals(session.Nonce, nonce, StringComparison.Ordinal);

    private static bool IsValidProductVersion(string? productVersion)
    {
        if (string.IsNullOrWhiteSpace(productVersion))
        {
            return false;
        }

        return productVersion!.Length <= MaximumProductVersionCharacters;
    }

    private static bool HasOnlyKnownCapabilities(CompanionCapability capabilities) =>
        (capabilities & ~KnownCapabilities) == CompanionCapability.None;

    private sealed class Session
    {
        public Session(long peerId, string nonce, DateTimeOffset handshakeDeadline)
        {
            PeerId = peerId;
            Nonce = nonce;
            HandshakeDeadline = handshakeDeadline;
        }

        public long PeerId { get; }

        public string Nonce { get; }

        public DateTimeOffset HandshakeDeadline { get; }

        public bool IsNegotiated { get; set; }

        public int? SelectedProtocolVersion { get; set; }

        public string? ProductVersion { get; set; }

        public CompanionCapability Capabilities { get; set; }

        public long LastSequence { get; set; }

        public DateTimeOffset? LastHeartbeat { get; set; }
    }
}
