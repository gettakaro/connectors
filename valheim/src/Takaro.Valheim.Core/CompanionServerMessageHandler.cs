using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

public sealed record CompanionMessageHandlingResult(
    CompanionReportOutput? Output,
    CompanionSessionDecision? SessionDecision,
    int? ReportedProtocolVersion,
    string? ReportedProductVersion);

public sealed class CompanionServerMessageHandler
{
    private readonly CompanionSessionRegistry sessions;
    private readonly CompanionRateLimiter rateLimiter;
    private readonly CompanionReportProcessor reportProcessor;

    public CompanionServerMessageHandler(
        CompanionSessionRegistry sessions,
        CompanionRateLimiter rateLimiter,
        CompanionReportProcessor reportProcessor)
    {
        this.sessions = sessions ?? throw new ArgumentNullException(nameof(sessions));
        this.rateLimiter = rateLimiter ?? throw new ArgumentNullException(nameof(rateLimiter));
        this.reportProcessor = reportProcessor ?? throw new ArgumentNullException(nameof(reportProcessor));
    }

    public CompanionReportOutput? Process(
        long peerId,
        TakaroPlayer player,
        string json,
        DateTimeOffset now) =>
        Handle(peerId, player, json, now).Output;

    public CompanionMessageHandlingResult Handle(
        long peerId,
        TakaroPlayer player,
        string json,
        DateTimeOffset now)
    {
        if (player is null
            || !CompanionEnvelopeCodec.TryDecodeEnvelope(json, out var envelope, out _)
            || envelope is null)
        {
            return EmptyResult();
        }

        switch (envelope.Type)
        {
            case CompanionMessageTypes.HelloAck:
            case CompanionMessageTypes.HelloNack:
                if (!rateLimiter.TryConsume(peerId, envelope.Type, now))
                {
                    return EmptyResult();
                }

                return envelope.Type == CompanionMessageTypes.HelloAck
                    ? ProcessHelloAck(peerId, envelope, now)
                    : ProcessHelloNack(peerId, envelope, now);
            case CompanionMessageTypes.Heartbeat:
                if (!rateLimiter.TryConsume(peerId, envelope.Type, now))
                {
                    return EmptyResult();
                }

                return ProcessHeartbeat(peerId, envelope, now);
            case CompanionMessageTypes.Chat:
            case CompanionMessageTypes.InventorySnapshot:
            case CompanionMessageTypes.PlayerDeath:
            case CompanionMessageTypes.EntityKilled:
                return new CompanionMessageHandlingResult(
                    reportProcessor.Process(peerId, player, envelope, now),
                    null,
                    null,
                    null);
            default:
                return EmptyResult();
        }
    }

    private CompanionMessageHandlingResult ProcessHelloAck(
        long peerId,
        CompanionEnvelope envelope,
        DateTimeOffset now)
    {
        var isStrictlyValid = CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloAck>(
            envelope,
            out var strictHelloAck,
            out _);
        if (!CompanionEnvelopeCodec.TryInspectHelloAck(
                envelope,
                out var inspectedHelloAck)
            || inspectedHelloAck is null)
        {
            return EmptyResult();
        }

        var helloAck = strictHelloAck ?? inspectedHelloAck;
        var isSupportedVersion = helloAck.ProtocolVersion >= CompanionProtocol.MinimumVersion
            && helloAck.ProtocolVersion <= CompanionProtocol.CurrentVersion;
        if ((isSupportedVersion && !isStrictlyValid)
            || (isSupportedVersion
                && envelope.ProtocolVersion != helloAck.ProtocolVersion))
        {
            return EmptyResult();
        }

        var decision = sessions.CompleteHelloAck(
            peerId,
            envelope.SessionNonce,
            helloAck.ProtocolVersion,
            helloAck.ProductVersion,
            helloAck.AcceptedCapabilities,
            envelope.Sequence,
            now);
        return new CompanionMessageHandlingResult(
            null,
            decision,
            helloAck.ProtocolVersion,
            helloAck.ProductVersion);
    }

    private CompanionMessageHandlingResult ProcessHeartbeat(
        long peerId,
        CompanionEnvelope envelope,
        DateTimeOffset now)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionHeartbeat>(
                envelope,
                out var heartbeat,
                out _)
            || heartbeat is null)
        {
            return EmptyResult();
        }

        var decision = sessions.ValidateHeartbeat(
            peerId,
            envelope.SessionNonce,
            envelope.ProtocolVersion,
            envelope.Sequence,
            now);
        return new CompanionMessageHandlingResult(
            null,
            decision,
            null,
            null);
    }

    private CompanionMessageHandlingResult ProcessHelloNack(
        long peerId,
        CompanionEnvelope envelope,
        DateTimeOffset now)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloNack>(
                envelope,
                out var helloNack,
                out _)
            || helloNack is null
            || CompanionVersionPolicy.TryNegotiate(
                CompanionProtocol.MinimumVersion,
                CompanionProtocol.CurrentVersion,
                helloNack.MinimumVersion,
                helloNack.MaximumVersion,
                out _))
        {
            return EmptyResult();
        }

        var decision = sessions.CompleteHelloAck(
            peerId,
            envelope.SessionNonce,
            helloNack.MaximumVersion,
            helloNack.ProductVersion,
            CompanionCapability.None,
            envelope.Sequence,
            now);
        return new CompanionMessageHandlingResult(
            null,
            decision,
            helloNack.MaximumVersion,
            helloNack.ProductVersion);
    }

    private static CompanionMessageHandlingResult EmptyResult() =>
        new(null, null, null, null);
}
