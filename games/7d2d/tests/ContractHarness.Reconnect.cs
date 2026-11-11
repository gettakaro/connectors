using System;
using Takaro.Config;
using Takaro.WebSocket;

// The reconnect delay policy (gettakaro/connectors#858, #860): a drop after a
// stable session is retried at once, only consecutive failures back off.
public static partial class ContractHarness
{
    private static double Seconds(int streak, int baseSeconds, double random)
    {
        return ReconnectPolicy.NextDelay(streak, baseSeconds, random).TotalSeconds;
    }

    private static void FamilyReconnectFastRetryAfterStableSession()
    {
        // Whatever the configured base, including the old written default of 30.
        foreach (int configured in new[] { 1, 2, 30, 60, 300 })
        {
            double low = Seconds(0, configured, 0);
            double high = Seconds(0, configured, 0.999999);
            True(low >= 0.5, $"base {configured}: the fast retry waits at least 0.5 s");
            True(high <= 1.25, $"base {configured}: the fast retry is under 1.25 s");
            True(high - low > 0.7, $"base {configured}: the fast retry is jittered");
        }
        True(
            Seconds(0, 30, 0.5) < 2,
            "a stable session's drop is retried within 2 s even with the old 30 s config"
        );
    }

    private static void FamilyReconnectBacksOffOnlyOnConsecutiveFailures()
    {
        int b = ConfigFiles.DefaultReconnectIntervalSeconds;
        Equal(2, b, "the default backoff base is 2 s");

        double previousCeiling = 0;
        for (int streak = 1; streak <= 7; streak++)
        {
            double ceiling = Seconds(streak, b, 1);
            double floor = Seconds(streak, b, 0);
            True(Math.Abs(floor - ceiling / 2) < 1e-9, $"streak {streak}: floor is half the ceiling");
            True(ceiling >= previousCeiling, $"streak {streak}: the delay does not shrink");
            previousCeiling = ceiling;
        }
        Equal(2.0, Seconds(1, b, 1), "the first failure waits up to the base");
        Equal(4.0, Seconds(2, b, 1), "the second failure doubles it");
        Equal(32.0, Seconds(5, b, 1), "the fifth failure waits up to 32 s");
        Equal(60.0, Seconds(6, b, 1), "the sixth failure hits the 60 s cap");
        Equal(60.0, Seconds(1000, b, 1), "a long streak stays at the cap");
        True(Seconds(1000, b, 0) >= 30, "jitter at the cap keeps at least 30 s");

        // An operator's override is the backoff base, still capped.
        Equal(30.0, Seconds(1, 30, 1), "a configured 30 s base is honoured");
        Equal(60.0, Seconds(2, 30, 1), "a configured base doubles to the cap");
        Equal(60.0, Seconds(1, 300, 1), "a base above the cap is capped");
        Equal(1.0, Seconds(1, 0, 1), "a zero base is treated as 1 s");
        Equal(0.5, Seconds(1, 1, -5), "a random value below 0 is clamped");
        Equal(1.0, Seconds(1, 1, 7), "a random value above 1 is clamped");
    }

    private static void FamilyReconnectStreakResetsOnlyAfterAStableConfirmedSession()
    {
        True(
            ReconnectPolicy.ClosedSessionWasStable(true, 10, 10),
            "confirmed and up for the stable window resets the streak"
        );
        True(
            ReconnectPolicy.ClosedSessionWasStable(true, 3600, 10),
            "a long confirmed session resets the streak"
        );
        True(
            !ReconnectPolicy.ClosedSessionWasStable(true, 9, 10),
            "a confirmed session that drops early keeps backing off"
        );
        True(
            !ReconnectPolicy.ClosedSessionWasStable(false, 3600, 10),
            "a socket that was never confirmed keeps backing off"
        );
    }
}
