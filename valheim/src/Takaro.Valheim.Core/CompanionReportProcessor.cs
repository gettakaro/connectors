using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

public abstract record CompanionReportOutput;

public sealed record CompanionInventoryUpdated(TakaroPlayer Player) : CompanionReportOutput;

public sealed record CompanionAcceptedEvent(string Type, object Data) : CompanionReportOutput;

public sealed class CompanionReportProcessor
{
    private readonly CompanionSessionRegistry sessions;
    private readonly CompanionRateLimiter rateLimiter;
    private readonly BoundedEventDeduplicator eventDeduplicator;
    private readonly CompanionInventoryCache inventory;

    public CompanionReportProcessor(
        CompanionSessionRegistry sessions,
        CompanionRateLimiter rateLimiter,
        BoundedEventDeduplicator eventDeduplicator,
        CompanionInventoryCache inventory)
    {
        this.sessions = sessions ?? throw new ArgumentNullException(nameof(sessions));
        this.rateLimiter = rateLimiter ?? throw new ArgumentNullException(nameof(rateLimiter));
        this.eventDeduplicator = eventDeduplicator ?? throw new ArgumentNullException(nameof(eventDeduplicator));
        this.inventory = inventory ?? throw new ArgumentNullException(nameof(inventory));
    }

    public CompanionReportOutput? Process(
        long peerId,
        TakaroPlayer player,
        CompanionEnvelope envelope,
        DateTimeOffset now)
    {
        if (player is null
            || envelope is null
            || !TryGetRequiredCapability(envelope.Type, out var requiredCapability))
        {
            return null;
        }

        if (sessions.ValidateReport(
                peerId,
                envelope.SessionNonce,
                envelope.ProtocolVersion,
                envelope.Sequence,
                now)
            != CompanionSessionDecision.Accept)
        {
            return null;
        }

        if (!sessions.TryGetSnapshot(peerId, out var session)
            || (session.Capabilities & requiredCapability) != requiredCapability
            || !rateLimiter.TryConsume(peerId, envelope.Type, now))
        {
            return null;
        }

        switch (envelope.Type)
        {
            case CompanionMessageTypes.Chat:
                return ProcessChat(peerId, player, envelope);
            case CompanionMessageTypes.InventorySnapshot:
                return ProcessInventory(peerId, player, envelope, now);
            case CompanionMessageTypes.PlayerDeath:
                return ProcessPlayerDeath(peerId, player, envelope);
            case CompanionMessageTypes.EntityKilled:
                return ProcessEntityKilled(peerId, player, envelope);
            default:
                return null;
        }
    }

    private CompanionReportOutput? ProcessChat(
        long peerId,
        TakaroPlayer player,
        CompanionEnvelope envelope)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionChatReport>(
                envelope,
                out var report,
                out _)
            || !TryTimestamp(report!.TimestampUnixMilliseconds, out var timestamp))
        {
            return null;
        }

        return AcceptEvent(
            peerId,
            report.EventId,
            ValheimEventType.ChatMessage,
            EventFactory.ChatMessage(player, "global", timestamp, report.Message));
    }

    private CompanionReportOutput? ProcessInventory(
        long peerId,
        TakaroPlayer player,
        CompanionEnvelope envelope,
        DateTimeOffset now)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionInventoryReport>(
                envelope,
                out var report,
                out _)
            || !inventory.Remember(
                peerId,
                envelope.SessionNonce,
                player,
                report!.Stacks,
                now))
        {
            return null;
        }

        return new CompanionInventoryUpdated(player);
    }

    private CompanionReportOutput? ProcessPlayerDeath(
        long peerId,
        TakaroPlayer player,
        CompanionEnvelope envelope)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionPlayerDeathReport>(
                envelope,
                out var report,
                out _)
            || !TryTimestamp(report!.TimestampUnixMilliseconds, out var timestamp))
        {
            return null;
        }

        return AcceptEvent(
            peerId,
            report.EventId,
            ValheimEventType.PlayerDeath,
            EventFactory.CompanionPlayerDeath(
                player,
                timestamp,
                Position(report.Position),
                report.CauseHint,
                report.AttackerCodeHint));
    }

    private CompanionReportOutput? ProcessEntityKilled(
        long peerId,
        TakaroPlayer player,
        CompanionEnvelope envelope)
    {
        if (!CompanionEnvelopeCodec.TryDecodePayload<CompanionEntityKilledReport>(
                envelope,
                out var report,
                out _)
            || string.IsNullOrWhiteSpace(report!.EntityCodeHint)
            || !TryTimestamp(report.TimestampUnixMilliseconds, out var timestamp))
        {
            return null;
        }

        return AcceptEvent(
            peerId,
            report.EventId,
            ValheimEventType.EntityKilled,
            EventFactory.EntityKilled(
                player,
                report.EntityCodeHint!,
                timestamp,
                Position(report.Position),
                report.WeaponCodeHint));
    }

    private CompanionReportOutput? AcceptEvent(
        long peerId,
        string eventId,
        string eventType,
        object data)
    {
        if (!ValheimEventAcceptancePolicy.CanEmit(
                eventType,
                ValheimEventObservationSource.ClientCompanion)
            || !eventDeduplicator.TryAccept(peerId, eventId))
        {
            return null;
        }

        return new CompanionAcceptedEvent(eventType, data);
    }

    private static TakaroPosition Position(CompanionPosition position) =>
        new(position.X, position.Y, position.Z, "valheim");

    private static bool TryTimestamp(long unixMilliseconds, out DateTimeOffset timestamp)
    {
        try
        {
            timestamp = DateTimeOffset.FromUnixTimeMilliseconds(unixMilliseconds);
            return true;
        }
        catch (ArgumentOutOfRangeException)
        {
            timestamp = default;
            return false;
        }
    }

    private static bool TryGetRequiredCapability(
        string messageType,
        out CompanionCapability capability)
    {
        switch (messageType)
        {
            case CompanionMessageTypes.Chat:
                capability = CompanionCapability.Chat;
                return true;
            case CompanionMessageTypes.InventorySnapshot:
                capability = CompanionCapability.Inventory;
                return true;
            case CompanionMessageTypes.PlayerDeath:
                capability = CompanionCapability.PlayerDeath;
                return true;
            case CompanionMessageTypes.EntityKilled:
                capability = CompanionCapability.EntityKilled;
                return true;
            default:
                capability = CompanionCapability.None;
                return false;
        }
    }
}
