#include "takaro/reconnect.h"

#include <algorithm>
#include <chrono>

namespace takaro {

ReconnectPolicy::Params ReconnectPolicy::FromConfig(unsigned baseMs, unsigned maxMs) {
    Params p;
    p.baseMs = std::max<int64_t>(1, baseMs);
    p.maxMs = std::max<int64_t>(p.baseMs, maxMs);
    p.fastMinMs = std::min(p.fastMinMs, p.baseMs);
    p.fastJitterMs = std::min(p.fastJitterMs, p.baseMs);
    return p;
}

ReconnectPolicy::ReconnectPolicy(Params p, uint64_t seed) : p_(p) {
    if (!seed) {
        std::random_device rd;
        seed = ((uint64_t)rd() << 32) ^ rd() ^
               (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    }
    rng_.seed(seed);
}

int64_t ReconnectPolicy::Uniform(int64_t lo, int64_t hi) {
    if (hi <= lo) return lo;
    return std::uniform_int_distribution<int64_t>(lo, hi)(rng_);
}

ReconnectPolicy::Delay ReconnectPolicy::OnEnded(int64_t nowMs) {
    Delay d;
    const bool stable = identifiedAtMs_ >= 0 && nowMs - identifiedAtMs_ >= p_.stableMs;
    identifiedAtMs_ = -1;
    if (stable) {
        streak_ = 0;
        d.fast = true;
        d.ms = p_.fastMinMs + Uniform(0, p_.fastJitterMs);
        return d;
    }
    streak_++;
    int64_t step = p_.baseMs;
    for (unsigned i = 1; i < streak_ && step < p_.maxMs; i++) step *= 2;
    step = std::min(step, p_.maxMs);
    d.streak = streak_;
    d.ms = Uniform(step / 2, step);
    return d;
}

}  // namespace takaro
