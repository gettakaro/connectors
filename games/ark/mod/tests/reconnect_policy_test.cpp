// Delay policy for reconnecting to Takaro (gettakaro/connectors#858): fast first retry
// after a stable session, jittered exponential backoff on consecutive failures, 60 s cap,
// streak reset only by a stable connection.
#include "../src/reconnect_policy.hpp"

#include <iostream>
#include <set>

using namespace std::chrono_literals;
using takaro::ReconnectPolicy;

static int failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
      ++failures;                                                                \
    }                                                                            \
  } while (0)

const auto t0 = ReconnectPolicy::clock::time_point{} + 1h;

void test_defaults() {
  const ReconnectPolicy::Settings s;
  CHECK(s.fast == 500ms && s.fast_jitter == 1000ms);
  CHECK(s.base == 2000ms && s.max == 60000ms);
  CHECK(s.stable_after == 10000ms);
}

void test_fast_retry_after_stable_session() {
  for (double u : {0.0, 0.5, 0.999999, 1.0}) {
    ReconnectPolicy p({}, [u] { return u; });
    p.identified(t0);
    const auto d = p.dropped(t0 + 10s);
    CHECK(d >= 500ms && d <= 1500ms);
    CHECK(p.streak() == 0);
  }
  // Real jitter: spread over the window, never outside it.
  ReconnectPolicy p({});
  std::set<long long> seen;
  for (int i = 0; i < 200; ++i) {
    p.identified(t0);
    const auto d = p.dropped(t0 + 1h);
    CHECK(d >= 500ms && d <= 1500ms);
    seen.insert(d.count());
  }
  CHECK(seen.size() > 50);
}

void test_unstable_drop_backs_off() {
  ReconnectPolicy p({}, [] { return 0.0; });
  p.identified(t0);
  CHECK(p.dropped(t0 + 9999ms) == 1000ms);  // identified but not stable yet: failure 1
  CHECK(p.streak() == 1);
  // A drop without identify (refused identify, closed during the handshake) is a failure too.
  CHECK(p.dropped(t0 + 20s) == 2000ms);
  CHECK(p.streak() == 2);
}

void test_streak_backoff_and_cap() {
  ReconnectPolicy lo({}, [] { return 0.0; });
  ReconnectPolicy hi({}, [] { return 1.0; });
  // Failure n: [ceiling/2, ceiling], ceiling = min(60 s, 2 s * 2^(n-1)).
  const long long ceilings[] = {2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
  for (long long ceiling : ceilings) {
    CHECK(lo.failed().count() == ceiling / 2);
    CHECK(hi.failed().count() == ceiling);
  }
  for (int i = 0; i < 100; ++i) CHECK(hi.failed() <= 60000ms);  // no overflow far into the streak
  ReconnectPolicy rnd({});
  for (int i = 0; i < 40; ++i) {
    const auto d = rnd.failed();
    CHECK(d >= 1000ms && d <= 60000ms);
  }
}

void test_jitter_spreads_backoff() {
  ReconnectPolicy p({});
  std::set<long long> seen;
  for (int i = 0; i < 100; ++i) seen.insert(p.backoff(6).count());
  CHECK(seen.size() > 50);
  for (long long v : seen) CHECK(v >= 30000 && v <= 60000);
}

void test_stable_session_resets_streak() {
  ReconnectPolicy p({}, [] { return 0.0; });
  for (int i = 0; i < 6; ++i) p.failed();
  CHECK(p.streak() == 6);
  p.identified(t0);
  // Identify alone does not reset the streak; a drop before stable keeps backing off.
  CHECK(p.dropped(t0 + 2s) == 30000ms);
  CHECK(p.streak() == 7);
  p.identified(t0 + 1min);
  CHECK(p.dropped(t0 + 1min + 10s) == 500ms);
  CHECK(p.streak() == 0);
  CHECK(p.failed() == 1000ms);  // a new streak starts from the base
}

void test_overrides() {
  ReconnectPolicy p({100ms, 0ms, 400ms, 1000ms, 300ms}, [] { return 1.0; });
  p.identified(t0);
  CHECK(p.dropped(t0 + 300ms) == 100ms);
  CHECK(p.failed() == 400ms);
  CHECK(p.failed() == 800ms);
  CHECK(p.failed() == 1000ms);
}

int main() {
  test_defaults();
  test_fast_retry_after_stable_session();
  test_unstable_drop_backs_off();
  test_streak_backoff_and_cap();
  test_jitter_spreads_backoff();
  test_stable_session_resets_streak();
  test_overrides();
  if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
  std::cout << "reconnect_policy_test passed\n";
  return 0;
}
