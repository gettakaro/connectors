namespace Takaro.Valheim.Core;

public enum CompanionEnforcementAction
{
    None,
    RestartSession,
    ExplainThenDisconnect
}

public enum CompanionEnforcementReason
{
    None,
    MissingCompanion,
    IncompatibleProtocol,
    HeartbeatExpired
}

public sealed record CompanionEnforcementDecision(
    CompanionEnforcementAction Action,
    CompanionEnforcementReason Reason,
    int ExpectedMinimumVersion,
    int ExpectedMaximumVersion,
    int? ActualProtocolVersion)
{
    public bool RequiresDisconnect =>
        Action == CompanionEnforcementAction.ExplainThenDisconnect;
}

public static class CompanionEnforcementPolicy
{
    public static CompanionEnforcementDecision Evaluate(
        CompanionMode mode,
        CompanionSessionSnapshot? session,
        DateTimeOffset now,
        int minimumProtocolVersion,
        int maximumProtocolVersion,
        int? reportedProtocolVersion)
    {
        if (!Enum.IsDefined(typeof(CompanionMode), mode))
        {
            throw new ArgumentOutOfRangeException(nameof(mode));
        }
        if (minimumProtocolVersion <= 0
            || maximumProtocolVersion < minimumProtocolVersion)
        {
            throw new ArgumentOutOfRangeException(
                nameof(minimumProtocolVersion),
                "Protocol version range must be positive and ordered.");
        }

        if (mode == CompanionMode.Disabled)
        {
            return Decision(CompanionEnforcementAction.None);
        }

        if (mode == CompanionMode.Required
            && reportedProtocolVersion is int actualVersion
            && (actualVersion < minimumProtocolVersion
                || actualVersion > maximumProtocolVersion))
        {
            return Decision(
                CompanionEnforcementAction.ExplainThenDisconnect,
                CompanionEnforcementReason.IncompatibleProtocol,
                actualVersion);
        }

        if (session is null)
        {
            return Decision(CompanionEnforcementAction.RestartSession);
        }

        if (now < session.ExpiresAt)
        {
            return Decision(CompanionEnforcementAction.None);
        }

        if (mode == CompanionMode.Optional)
        {
            return Decision(CompanionEnforcementAction.RestartSession);
        }

        return Decision(
            CompanionEnforcementAction.ExplainThenDisconnect,
            session.IsNegotiated
                ? CompanionEnforcementReason.HeartbeatExpired
                : CompanionEnforcementReason.MissingCompanion,
            reportedProtocolVersion);

        CompanionEnforcementDecision Decision(
            CompanionEnforcementAction action,
            CompanionEnforcementReason reason = CompanionEnforcementReason.None,
            int? actual = null) =>
            new(
                action,
                reason,
                minimumProtocolVersion,
                maximumProtocolVersion,
                actual);
    }
}

public enum CompanionDisconnectStep
{
    None,
    Explain,
    Kick,
    ForceDisconnect
}

public sealed class CompanionDisconnectSchedule
{
    private readonly TimeSpan explanationGrace;
    private readonly TimeSpan fallbackGrace;
    private readonly object syncRoot = new();
    private readonly DateTimeOffset startsAt;
    private bool explanationRetryPending;
    private int stage;
    private DateTimeOffset kickAt;
    private DateTimeOffset forceDisconnectAt;

    public CompanionDisconnectSchedule(
        CompanionEnforcementDecision decision,
        DateTimeOffset startsAt,
        TimeSpan explanationGrace,
        TimeSpan fallbackGrace)
    {
        if (decision is null || !decision.RequiresDisconnect)
        {
            throw new ArgumentException(
                "A disconnect schedule requires an enforcement decision that disconnects.",
                nameof(decision));
        }
        if (explanationGrace <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(explanationGrace));
        }
        if (fallbackGrace <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(fallbackGrace));
        }

        Decision = decision;
        this.startsAt = startsAt;
        this.explanationGrace = explanationGrace;
        this.fallbackGrace = fallbackGrace;
    }

    public CompanionEnforcementDecision Decision { get; }

    public CompanionDisconnectStep TakeDueStep(DateTimeOffset now)
    {
        lock (syncRoot)
        {
            if (stage == 0 && now >= startsAt)
            {
                stage = 1;
                kickAt = SaturatingAdd(now, explanationGrace);
                return CompanionDisconnectStep.Explain;
            }

            if (stage == 1 && now >= kickAt)
            {
                stage = 2;
                forceDisconnectAt = SaturatingAdd(now, fallbackGrace);
                return CompanionDisconnectStep.Kick;
            }

            if (stage == 1 && explanationRetryPending)
            {
                explanationRetryPending = false;
                return CompanionDisconnectStep.Explain;
            }

            if (stage == 2 && now >= forceDisconnectAt)
            {
                stage = 3;
                return CompanionDisconnectStep.ForceDisconnect;
            }

            return CompanionDisconnectStep.None;
        }
    }

    public void RetryForceDisconnect(DateTimeOffset now)
    {
        lock (syncRoot)
        {
            if (stage != 3)
            {
                return;
            }

            stage = 2;
            forceDisconnectAt = SaturatingAdd(now, fallbackGrace);
        }
    }

    public void RetryExplanation()
    {
        lock (syncRoot)
        {
            if (stage == 1)
            {
                explanationRetryPending = true;
            }
        }
    }

    private static DateTimeOffset SaturatingAdd(
        DateTimeOffset value,
        TimeSpan duration) =>
        value > DateTimeOffset.MaxValue - duration
            ? DateTimeOffset.MaxValue
            : value + duration;
}

public sealed class CompanionNegotiationObservation
{
    public int? ReportedProtocolVersion { get; private set; }

    public int? RejectedProtocolVersion { get; private set; }

    public string? ReportedProductVersion { get; private set; }

    public void Observe(CompanionMessageHandlingResult result)
    {
        if (result is null
            || result.ReportedProtocolVersion is not int reportedProtocolVersion)
        {
            return;
        }

        if (result.SessionDecision == CompanionSessionDecision.RejectVersion)
        {
            RejectedProtocolVersion ??= reportedProtocolVersion;
            ReportedProductVersion ??= result.ReportedProductVersion;
            return;
        }

        if (result.SessionDecision == CompanionSessionDecision.Accept)
        {
            ReportedProtocolVersion = reportedProtocolVersion;
            ReportedProductVersion = result.ReportedProductVersion;
        }
    }
}
