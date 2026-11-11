#pragma once

#include <algorithm>
#include <cstdint>

// Reconnect delay policy (gettakaro/connectors#858), kept free of sockets and clocks so the unit
// tests can drive it directly.
//
// - A connection that was identified and stayed up for kStableMs is "stable". When it drops, the
//   next dial waits only kFastMinMs plus up to kFastJitterMs of jitter, and the failure streak
//   starts over.
// - Every other close or failed dial extends the streak: equal-jitter exponential backoff from
//   baseMs, doubling per failure, never above maxMs.
namespace ReconnectPolicy {
// 200-800 ms: TLS plus Takaro's identify round trip take up to ~1 s more, so the server is
// identified again within 2 s, and the jitter still spreads a fleet that reconnects after one
// edge restart.
constexpr unsigned kFastMinMs = 200;
constexpr unsigned kFastJitterMs = 600;
constexpr unsigned kStableMs = 10000;

class Backoff {
public:
    Backoff(unsigned baseMs, unsigned maxMs)
        : base_(std::max(1u, baseMs)), max_(std::max(std::max(1u, baseMs), maxMs)) {}

    // Delay before the next dial. `stable`: the closed connection counted as stable.
    // `random` is any uniformly distributed 32-bit value.
    unsigned NextDelayMs(bool stable, uint32_t random) {
        if (stable) {
            streak_ = 0;
            return kFastMinMs + random % (kFastJitterMs + 1);
        }
        ++streak_;
        uint64_t nominal = base_;
        for (unsigned i = 1; i < streak_ && nominal < max_; ++i) nominal *= 2;
        nominal = std::min<uint64_t>(nominal, max_);
        const uint64_t half = nominal / 2;
        return static_cast<unsigned>(nominal - half + random % (half + 1));
    }
    // A settings change dials at once and starts a fresh streak.
    void Reset() { streak_ = 0; }
    unsigned Streak() const { return streak_; }

private:
    unsigned base_, max_;
    unsigned streak_ = 0;
};
} // namespace ReconnectPolicy
