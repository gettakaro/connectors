using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

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
        DateTimeOffset now)
    {
        if (player is null
            || !CompanionEnvelopeCodec.TryDecodeEnvelope(json, out var envelope, out _)
            || envelope is null)
        {
            return null;
        }

        switch (envelope.Type)
        {
            case CompanionMessageTypes.HelloAck:
                if (!rateLimiter.TryConsume(peerId, envelope.Type, now))
                {
                    return null;
                }

                ProcessHelloAck(peerId, envelope, now);
                return null;
            case CompanionMessageTypes.Heartbeat:
                if (!rateLimiter.TryConsume(peerId, envelope.Type, now))
                {
                    return null;
                }

                ProcessHeartbeat(peerId, envelope, now);
                return null;
            case CompanionMessageTypes.Chat:
            case CompanionMessageTypes.InventorySnapshot:
            case CompanionMessageTypes.PlayerDeath:
            case CompanionMessageTypes.EntityKilled:
                return reportProcessor.Process(peerId, player, envelope, now);
            default:
                return null;
        }
    }

    private void ProcessHelloAck(
        long peerId,
        CompanionEnvelope envelope,
        DateTimeOffset now)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionHelloAck>(
                envelope,
                out var helloAck,
                out _)
            || helloAck is null
            || envelope.ProtocolVersion != helloAck.ProtocolVersion)
        {
            return;
        }

        sessions.CompleteHelloAck(
            peerId,
            envelope.SessionNonce,
            helloAck.ProtocolVersion,
            helloAck.ProductVersion,
            helloAck.AcceptedCapabilities,
            envelope.Sequence,
            now);
    }

    private void ProcessHeartbeat(
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
            return;
        }

        sessions.ValidateHeartbeat(
            peerId,
            envelope.SessionNonce,
            envelope.ProtocolVersion,
            envelope.Sequence,
            now);
    }
}
