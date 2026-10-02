using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

public enum CompanionMessageOutcome
{
    Ignored,
    Negotiated,
    VersionRejected,
    Heartbeat,
    InventoryAccepted,
    KillVerdictAccepted
}

public sealed record CompanionMessageHandlingResult(
    CompanionMessageOutcome Outcome,
    CompanionSessionDecision? SessionDecision = null,
    int? RemoteMinimumVersion = null,
    int? RemoteMaximumVersion = null,
    string? RemoteProductVersion = null,
    int? InventoryStacks = null,
    CompanionKillVerdict? KillVerdict = null)
{
    public static readonly CompanionMessageHandlingResult Ignored = new(CompanionMessageOutcome.Ignored);
}

/// <summary>
/// Server-side handling of one companion envelope. The caller has already bound the envelope
/// to an authenticated, ready network peer (<paramref name="peerId"/>) and resolved that
/// peer's player; nothing in a payload can claim an identity. Everything accepted here is
/// client-reported and untrusted.
/// </summary>
public sealed class CompanionServerMessageHandler
{
    private readonly CompanionSessionRegistry sessions;
    private readonly CompanionRateLimiter rateLimiter;
    private readonly CompanionRateLimiter killVerdictRateLimiter;
    private readonly CompanionInventoryCache inventory;
    private readonly CompanionKillVerdictStore killVerdicts;

    public CompanionServerMessageHandler(
        CompanionSessionRegistry sessions,
        CompanionRateLimiter rateLimiter,
        CompanionRateLimiter killVerdictRateLimiter,
        CompanionInventoryCache inventory,
        CompanionKillVerdictStore killVerdicts)
    {
        this.sessions = sessions ?? throw new ArgumentNullException(nameof(sessions));
        this.rateLimiter = rateLimiter ?? throw new ArgumentNullException(nameof(rateLimiter));
        this.killVerdictRateLimiter = killVerdictRateLimiter ?? throw new ArgumentNullException(nameof(killVerdictRateLimiter));
        this.inventory = inventory ?? throw new ArgumentNullException(nameof(inventory));
        this.killVerdicts = killVerdicts ?? throw new ArgumentNullException(nameof(killVerdicts));
    }

    /// <param name="now">Session clock (monotonic-anchored) used for handshakes and heartbeats.</param>
    /// <param name="receivedAt">Wall-clock receive time stamped on inventory snapshots and kill verdicts.</param>
    public CompanionMessageHandlingResult Handle(
        long peerId,
        TakaroPlayer player,
        string json,
        DateTimeOffset now,
        DateTimeOffset receivedAt)
    {
        if (player is null
            || !CompanionEnvelopeCodec.TryDecodeEnvelope(json, out var envelope, out _)
            || envelope is null)
        {
            return CompanionMessageHandlingResult.Ignored;
        }

        switch (envelope.Type)
        {
            case CompanionMessageTypes.HelloAck:
                return rateLimiter.TryConsume(peerId, envelope.Type, now)
                    ? ProcessHelloAck(peerId, envelope, now)
                    : CompanionMessageHandlingResult.Ignored;
            case CompanionMessageTypes.HelloNack:
                return rateLimiter.TryConsume(peerId, envelope.Type, now)
                    ? ProcessHelloNack(peerId, envelope, now)
                    : CompanionMessageHandlingResult.Ignored;
            case CompanionMessageTypes.Heartbeat:
                return rateLimiter.TryConsume(peerId, envelope.Type, now)
                    ? ProcessHeartbeat(peerId, envelope, now)
                    : CompanionMessageHandlingResult.Ignored;
            case CompanionMessageTypes.InventorySnapshot:
                return ProcessInventory(peerId, player, envelope, now, receivedAt);
            case CompanionMessageTypes.KillVerdict:
                return ProcessKillVerdict(peerId, envelope, now, receivedAt);
            default:
                return CompanionMessageHandlingResult.Ignored;
        }
    }

    private CompanionMessageHandlingResult ProcessHelloAck(
        long peerId,
        CompanionEnvelope envelope,
        DateTimeOffset now)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloAck>(envelope, out var helloAck, out _)
            || helloAck is null)
        {
            return CompanionMessageHandlingResult.Ignored;
        }

        var supported = helloAck.ProtocolVersion >= CompanionProtocol.MinimumVersion
            && helloAck.ProtocolVersion <= CompanionProtocol.CurrentVersion;
        if (supported && envelope.ProtocolVersion != helloAck.ProtocolVersion)
        {
            return CompanionMessageHandlingResult.Ignored;
        }

        var decision = sessions.CompleteHelloAck(
            peerId,
            envelope.SessionNonce,
            helloAck.ProtocolVersion,
            helloAck.ProductVersion,
            helloAck.AcceptedCapabilities,
            envelope.Sequence,
            now);
        if (decision == CompanionSessionDecision.Accept)
        {
            inventory.BeginSession(peerId, envelope.SessionNonce);
        }

        return new CompanionMessageHandlingResult(
            decision switch
            {
                CompanionSessionDecision.Accept => CompanionMessageOutcome.Negotiated,
                CompanionSessionDecision.RejectVersion => CompanionMessageOutcome.VersionRejected,
                _ => CompanionMessageOutcome.Ignored
            },
            decision,
            helloAck.ProtocolVersion,
            helloAck.ProtocolVersion,
            helloAck.ProductVersion);
    }

    private CompanionMessageHandlingResult ProcessHelloNack(
        long peerId,
        CompanionEnvelope envelope,
        DateTimeOffset now)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloNack>(envelope, out var helloNack, out _)
            || helloNack is null
            || CompanionVersionPolicy.TryNegotiate(
                CompanionProtocol.MinimumVersion,
                CompanionProtocol.CurrentVersion,
                helloNack.MinimumVersion,
                helloNack.MaximumVersion,
                out _))
        {
            // A nack whose range overlaps ours is a confused or hostile client: ignore it.
            return CompanionMessageHandlingResult.Ignored;
        }

        var decision = sessions.RecordIncompatible(
            peerId,
            envelope.SessionNonce,
            helloNack.MinimumVersion,
            helloNack.MaximumVersion,
            now);
        return new CompanionMessageHandlingResult(
            decision == CompanionSessionDecision.RejectVersion
                ? CompanionMessageOutcome.VersionRejected
                : CompanionMessageOutcome.Ignored,
            decision,
            helloNack.MinimumVersion,
            helloNack.MaximumVersion,
            helloNack.ProductVersion);
    }

    private CompanionMessageHandlingResult ProcessHeartbeat(
        long peerId,
        CompanionEnvelope envelope,
        DateTimeOffset now)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionHeartbeat>(envelope, out var heartbeat, out _)
            || heartbeat is null)
        {
            return CompanionMessageHandlingResult.Ignored;
        }

        var decision = sessions.ValidateHeartbeat(
            peerId,
            envelope.SessionNonce,
            envelope.ProtocolVersion,
            envelope.Sequence,
            now);
        return new CompanionMessageHandlingResult(
            decision == CompanionSessionDecision.Accept
                ? CompanionMessageOutcome.Heartbeat
                : CompanionMessageOutcome.Ignored,
            decision);
    }

    private CompanionMessageHandlingResult ProcessInventory(
        long peerId,
        TakaroPlayer player,
        CompanionEnvelope envelope,
        DateTimeOffset now,
        DateTimeOffset receivedAt)
    {
        if (!TryValidateReport(peerId, envelope, now, rateLimiter, CompanionCapability.Inventory, out var decision))
        {
            return new CompanionMessageHandlingResult(CompanionMessageOutcome.Ignored, decision);
        }

        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionInventoryReport>(envelope, out var report, out _)
            || report is null
            || !inventory.Remember(peerId, envelope.SessionNonce, player, report.Stacks, receivedAt))
        {
            return new CompanionMessageHandlingResult(CompanionMessageOutcome.Ignored, decision);
        }

        return new CompanionMessageHandlingResult(
            CompanionMessageOutcome.InventoryAccepted,
            decision,
            InventoryStacks: report.Stacks.Count);
    }

    private CompanionMessageHandlingResult ProcessKillVerdict(
        long peerId,
        CompanionEnvelope envelope,
        DateTimeOffset now,
        DateTimeOffset receivedAt)
    {
        if (!TryValidateReport(peerId, envelope, now, killVerdictRateLimiter, CompanionCapability.None, out var decision))
        {
            return new CompanionMessageHandlingResult(CompanionMessageOutcome.Ignored, decision);
        }

        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionKillVerdict>(envelope, out var verdict, out _)
            || verdict is null
            || !killVerdicts.Add(peerId, verdict, receivedAt))
        {
            return new CompanionMessageHandlingResult(CompanionMessageOutcome.Ignored, decision);
        }

        return new CompanionMessageHandlingResult(
            CompanionMessageOutcome.KillVerdictAccepted,
            decision,
            KillVerdict: verdict);
    }

    // Session validation advances the sequence before the rate and payload checks, so an
    // accepted envelope is processed at most once even when a later guard drops it.
    private bool TryValidateReport(
        long peerId,
        CompanionEnvelope envelope,
        DateTimeOffset now,
        CompanionRateLimiter limiter,
        CompanionCapability requiredCapability,
        out CompanionSessionDecision decision)
    {
        decision = sessions.ValidateReport(
            peerId,
            envelope.SessionNonce,
            envelope.ProtocolVersion,
            envelope.Sequence,
            now,
            out var session);
        return decision == CompanionSessionDecision.Accept
            && session is not null
            && (session.Capabilities & requiredCapability) == requiredCapability
            && limiter.TryConsume(peerId, envelope.Type, now);
    }
}
