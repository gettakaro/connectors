// Host unit tests for the platform-independent part of the transport (ReconnectPolicy, Heartbeat).
// g++ -std=c++20 -Isrc tests/transport_test.cpp src/transport.cpp -o transport_test && ./transport_test
#include "transport.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using native::Heartbeat;
using native::ReconnectPolicy;

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static void FastRetryAfterStableSession() {
    // 0.3 s plus up to 0.7 s of jitter: back well within Takaro's reachability window.
    CHECK(ReconnectPolicy::NextDelayMs(0, 0.0) == 300);
    CHECK(ReconnectPolicy::NextDelayMs(0, 0.5) == 650);
    CHECK(ReconnectPolicy::NextDelayMs(0, 0.999999) <= 1000);
    for (int i = 0; i < 1000; i++) {
        int64_t d = ReconnectPolicy::NextDelayMs(0, i / 1000.0);
        CHECK(d >= 300 && d <= 1000);
    }
    // The fast retry does not depend on the backoff base.
    CHECK(ReconnectPolicy::NextDelayMs(0, 0.0, 30000) == 300);
}

static void StreakBacksOffExponentially() {
    // Equal jitter: [ceiling/2, ceiling] with ceiling = base * 2^(streak-1).
    int64_t ceilings[] = {2000, 4000, 8000, 16000, 32000, 60000, 60000};
    for (unsigned s = 1; s <= 7; s++) {
        int64_t c = ceilings[s - 1];
        CHECK(ReconnectPolicy::NextDelayMs(s, 0.0) == c / 2);
        CHECK(ReconnectPolicy::NextDelayMs(s, 0.999999) <= c);
        CHECK(ReconnectPolicy::NextDelayMs(s, 0.999999) >= c - 1);
    }
    // Each step's lower bound is at least the previous step's lower bound: the streak never shortens.
    for (unsigned s = 1; s < 40; s++) CHECK(ReconnectPolicy::NextDelayMs(s + 1, 0.0) >= ReconnectPolicy::NextDelayMs(s, 0.0));
}

static void CappedAtSixtySeconds() {
    for (unsigned s = 0; s < 200; s++)
        for (double r : {0.0, 0.3, 0.999999, 1.0, 5.0, -1.0}) CHECK(ReconnectPolicy::NextDelayMs(s, r) <= 60000);
    CHECK(ReconnectPolicy::NextDelayMs(4000000000u, 1.0) == 60000);
    // A base above the cap is clamped to it.
    CHECK(ReconnectPolicy::NextDelayMs(1, 1.0, 600000) == 60000);
}

static void JitterSpreadsDelays() {
    // A Takaro deploy drops every server at once; their retries must not land on the same millisecond.
    CHECK(ReconnectPolicy::NextDelayMs(0, 0.1) != ReconnectPolicy::NextDelayMs(0, 0.9));
    CHECK(ReconnectPolicy::NextDelayMs(3, 0.1) != ReconnectPolicy::NextDelayMs(3, 0.9));
    CHECK(ReconnectPolicy::NextDelayMs(3, 0.9) - ReconnectPolicy::NextDelayMs(3, 0.1) >= 3000);
    // Out-of-range randomness is clamped.
    CHECK(ReconnectPolicy::NextDelayMs(0, -3.0) == 300);
    CHECK(ReconnectPolicy::NextDelayMs(0, 7.0) == 1000);
    CHECK(ReconnectPolicy::NextDelayMs(0, std::nan("")) == 300);
}

static void StreakResetsOnlyAfterStableIdentifiedSession() {
    CHECK(ReconnectPolicy::SessionWasStable(true, ReconnectPolicy::kStableMs));
    CHECK(ReconnectPolicy::SessionWasStable(true, 3600000));
    CHECK(!ReconnectPolicy::SessionWasStable(true, ReconnectPolicy::kStableMs - 1));  // accepted, then dropped
    CHECK(!ReconnectPolicy::SessionWasStable(false, 3600000));                        // never identified
    CHECK(ReconnectPolicy::kStableMs <= 10000);
}

static void SupervisorSequence() {
    // The transport's own bookkeeping: streak = failures since the last stable session.
    unsigned streak = 0;
    auto next = [&](bool identified, int64_t upMs) {
        if (ReconnectPolicy::SessionWasStable(identified, upMs)) streak = 0;
        int64_t d = ReconnectPolicy::NextDelayMs(streak, 1.0);
        streak++;
        return d;
    };
    CHECK(next(true, 600000) <= 1000);  // established session dropped: fast
    CHECK(next(false, 0) == 2000);      // that retry failed: back off
    CHECK(next(false, 0) == 4000);
    CHECK(next(true, 1000) == 8000);    // identified but dropped at once: still a failure
    CHECK(next(true, 30000) <= 1000);   // stable again: the streak is reset
}

static void HeartbeatDetectsDeadLink() {
    Heartbeat::Params p;
    Heartbeat hb(p);
    hb.Reset(0);
    CHECK(!hb.Dead(p.idleMs));
    CHECK(hb.Dead(p.idleMs + 1));
    hb.OnInbound(10000);
    CHECK(!hb.Dead(10000 + p.idleMs));
    // Pings go out well inside the idle window, so a live Takaro always answers in time.
    CHECK(p.intervalMs * 3 <= p.idleMs);
    CHECK(hb.PingDue(p.intervalMs, true));
}

static void Run(const char* name, void (*fn)()) {
    int before = failures;
    fn();
    std::printf("  %s %s\n", failures == before ? "ok  " : "FAIL", name);
}

int main() {
    Run("FastRetryAfterStableSession", FastRetryAfterStableSession);
    Run("StreakBacksOffExponentially", StreakBacksOffExponentially);
    Run("CappedAtSixtySeconds", CappedAtSixtySeconds);
    Run("JitterSpreadsDelays", JitterSpreadsDelays);
    Run("StreakResetsOnlyAfterStableIdentifiedSession", StreakResetsOnlyAfterStableIdentifiedSession);
    Run("SupervisorSequence", SupervisorSequence);
    Run("HeartbeatDetectsDeadLink", HeartbeatDetectsDeadLink);
    if (failures) {
        std::printf("%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    std::printf("ALL NATIVE TESTS PASSED\n");
    return 0;
}
