#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

// When to dial Takaro again after a connection ends (gettakaro/connectors#858).
//
// A connection whose identify Takaro accepted and that then stayed up for `stableMs` is
// "established": losing it is retried after fastMinMs + [0, fastJitterMs] and the failure streak
// resets. TLS plus identify take another 0.7-0.9 s through Takaro's edge, so the retry window stays
// small enough to be identified again within 2 s, and jittered so a Takaro deploy does not bring
// every server back at the same instant. Every other end (connect failed, closed before identify, dropped within stableMs) adds to
// the streak: base * 2^(streak-1), capped at maxMs, each delay a random point in [d/2, d].
// Header-only and free of libwebsockets so the unit tests can drive it. Not thread-safe.
class ReconnectPolicy {
public:
    struct Params {
        unsigned baseMs = 2000, maxMs = 60000; // TAKARO_RECONNECT_BASE_MS / _MAX_MS
        unsigned fastMinMs = 100, fastJitterMs = 400;
        int64_t stableMs = 10000; // like 7D2D's CONNECTION_STABLE_SECONDS
    };
    ReconnectPolicy() : ReconnectPolicy(Params{}, 0) {}
    // seed 0 = seed from the clock and the object address
    ReconnectPolicy(Params p, uint64_t seed) : p_(p) {
        if (p_.maxMs < p_.baseMs) p_.maxMs = p_.baseMs;
        if (!seed)
            seed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                   static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) ^
                   static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
        rng_ = seed ? seed : 0x9E3779B97F4A7C15ull;
    }
    // Identify accepted on the current connection.
    void OnIdentified(int64_t nowMs) { identifiedAtMs_ = nowMs; }
    // New connection settings: the streak no longer applies.
    void Reset() { streak_ = 0; identifiedAtMs_ = -1; }
    // Call exactly once per ended connection or failed attempt. Returns the delay before the next
    // attempt and whether the ended connection counted as established.
    unsigned NextDelayMs(int64_t nowMs, bool* established = nullptr) {
        const bool stable = identifiedAtMs_ >= 0 && nowMs - identifiedAtMs_ >= p_.stableMs;
        identifiedAtMs_ = -1;
        if (established) *established = stable;
        if (stable) {
            streak_ = 0;
            return p_.fastMinMs + Rand(p_.fastJitterMs);
        }
        ++streak_;
        const uint64_t d = static_cast<uint64_t>(p_.baseMs) << std::min(streak_ - 1, 20u);
        const unsigned capped = static_cast<unsigned>(std::min<uint64_t>(d, p_.maxMs));
        return capped - Rand(capped / 2);
    }
    unsigned Streak() const { return streak_; }

private:
    unsigned Rand(unsigned maxInclusive) {
        // xorshift64*: jitter only
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        const uint64_t r = rng_ * 0x2545F4914F6CDD1Dull;
        return static_cast<unsigned>((r >> 11) % (static_cast<uint64_t>(maxInclusive) + 1));
    }
    Params p_;
    uint64_t rng_;
    unsigned streak_ = 0;
    int64_t identifiedAtMs_ = -1;
};
