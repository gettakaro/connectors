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
    DateTimeOffset ExpiresAt,
    bool IsRejected = false,
    int? RejectedMinimumVersion = null,
    int? RejectedMaximumVersion = null);

public sealed class CompanionSessionRegistry
{
    private const CompanionCapability KnownCapabilities = CompanionCapability.Inventory;

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
        if (string.IsNullOrWhiteSpace(nonce)
            || nonce.Length > CompanionEnvelopeCodec.MaximumSessionNonceCharacters)
        {
            throw new ArgumentException(
                $"Session nonce must contain 1 to {CompanionEnvelopeCodec.MaximumSessionNonceCharacters} characters.",
                nameof(nonce));
        }

        var deadline = SaturatingAdd(now, handshakeGrace);
        lock (syncRoot)
        {
            sessions[peerId] = new Session(peerId, nonce, now, deadline);
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
            if (session.IsNegotiated || session.IsRejected)
            {
                return CompanionSessionDecision.RejectSequence;
            }
            if (selectedProtocolVersion < minimumProtocolVersion
                || selectedProtocolVersion > maximumProtocolVersion)
            {
                session.IsRejected = true;
                session.RejectedMinimumVersion = selectedProtocolVersion;
                session.RejectedMaximumVersion = selectedProtocolVersion;
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
            session.LastHeartbeat = LaterOf(session.BeginAt, now);
            return CompanionSessionDecision.Accept;
        }
    }

    public CompanionSessionDecision ValidateReport(
        long peerId,
        string nonce,
        int protocolVersion,
        long sequence,
        DateTimeOffset now) =>
        ValidateReport(
            peerId,
            nonce,
            protocolVersion,
            sequence,
            now,
            out _);

    public CompanionSessionDecision ValidateReport(
        long peerId,
        string nonce,
        int protocolVersion,
        long sequence,
        DateTimeOffset now,
        out CompanionSessionSnapshot? acceptedSession)
    {
        lock (syncRoot)
        {
            acceptedSession = null;
            var decision = ValidateNegotiatedEnvelopeLocked(
                peerId,
                nonce,
                protocolVersion,
                sequence,
                now,
                refreshHeartbeat: false);
            if (decision == CompanionSessionDecision.Accept)
            {
                acceptedSession = CreateSnapshot(sessions[peerId]);
            }

            return decision;
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

    /// <summary>
    /// Latches a version mismatch the companion reported for the current session (or that
    /// its hello-ack revealed). A rejected session is never negotiated and never retried
    /// until the peer reconnects; the player stays connected and plays vanilla.
    /// </summary>
    public CompanionSessionDecision RecordIncompatible(
        long peerId,
        string nonce,
        int remoteMinimumVersion,
        int remoteMaximumVersion,
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
            if (session.IsNegotiated || session.IsRejected)
            {
                return CompanionSessionDecision.RejectSequence;
            }
            if (IsExpired(session, now))
            {
                return CompanionSessionDecision.Expired;
            }

            session.IsRejected = true;
            session.RejectedMinimumVersion = remoteMinimumVersion;
            session.RejectedMaximumVersion = remoteMaximumVersion;
            return CompanionSessionDecision.RejectVersion;
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

    /// <summary>True while the peer has a negotiated session whose heartbeat is fresh.</summary>
    public bool IsActive(long peerId, DateTimeOffset now)
    {
        lock (syncRoot)
        {
            return sessions.TryGetValue(peerId, out var session)
                && session.IsNegotiated
                && !IsExpired(session, now);
        }
    }

    public bool TryGetActiveSession(
        long peerId,
        CompanionCapability requiredCapability,
        DateTimeOffset now,
        out CompanionSessionSnapshot snapshot)
    {
        lock (syncRoot)
        {
            if (requiredCapability == CompanionCapability.None
                || !HasOnlyKnownCapabilities(requiredCapability)
                || !sessions.TryGetValue(peerId, out var session)
                || !session.IsNegotiated
                || IsExpired(session, now)
                || (session.Capabilities & requiredCapability) != requiredCapability)
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

    /// <summary>
    /// Switches to a caller-supplied stable, immutable world value key.
    /// Value equality defines world equivalence, so an equal key preserves current sessions.
    /// </summary>
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
            session.LastHeartbeat = LaterOf(session.LastHeartbeat!.Value, now);
        }

        return CompanionSessionDecision.Accept;
    }

    private bool IsExpired(Session session, DateTimeOffset now)
    {
        if (!session.IsNegotiated)
        {
            return now >= session.HandshakeDeadline;
        }

        return now >= SaturatingAdd(session.LastHeartbeat!.Value, heartbeatGrace);
    }

    private CompanionSessionSnapshot CreateSnapshot(Session session)
    {
        var expiresAt = session.IsNegotiated
            ? SaturatingAdd(session.LastHeartbeat!.Value, heartbeatGrace)
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
            expiresAt,
            session.IsRejected,
            session.RejectedMinimumVersion,
            session.RejectedMaximumVersion);
    }

    private static bool NonceMatches(Session session, string nonce) =>
        string.Equals(session.Nonce, nonce, StringComparison.Ordinal);

    private static DateTimeOffset LaterOf(DateTimeOffset first, DateTimeOffset second) =>
        first >= second ? first : second;

    private static DateTimeOffset SaturatingAdd(DateTimeOffset value, TimeSpan duration)
    {
        try
        {
            return value + duration;
        }
        catch (ArgumentOutOfRangeException)
        {
            return DateTimeOffset.MaxValue;
        }
    }

    private static bool IsValidProductVersion(string? productVersion)
    {
        if (string.IsNullOrWhiteSpace(productVersion))
        {
            return false;
        }

        return productVersion!.Length <= CompanionProtocol.MaximumProductVersionCharacters;
    }

    private static bool HasOnlyKnownCapabilities(CompanionCapability capabilities) =>
        (capabilities & ~KnownCapabilities) == CompanionCapability.None;

    private sealed class Session
    {
        public Session(
            long peerId,
            string nonce,
            DateTimeOffset beginAt,
            DateTimeOffset handshakeDeadline)
        {
            PeerId = peerId;
            Nonce = nonce;
            BeginAt = beginAt;
            HandshakeDeadline = handshakeDeadline;
        }

        public long PeerId { get; }

        public string Nonce { get; }

        public DateTimeOffset BeginAt { get; }

        public DateTimeOffset HandshakeDeadline { get; }

        public bool IsNegotiated { get; set; }

        public int? SelectedProtocolVersion { get; set; }

        public string? ProductVersion { get; set; }

        public CompanionCapability Capabilities { get; set; }

        public long LastSequence { get; set; }

        public DateTimeOffset? LastHeartbeat { get; set; }

        public bool IsRejected { get; set; }

        public int? RejectedMinimumVersion { get; set; }

        public int? RejectedMaximumVersion { get; set; }
    }
}
