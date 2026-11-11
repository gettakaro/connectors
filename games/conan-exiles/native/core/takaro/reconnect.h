// Reconnect delay policy shared by every transport (Linux libwebsockets, Windows WinHTTP).
//
// An identified connection that stayed up for `stableMs` and then drops is redialled almost at
// once: fastMinMs plus a random 0..fastJitterMs, so a Takaro restart is not a thundering herd.
// Everything else (a refused dial, a TLS or handshake failure, an identify rejection, a connection
// that drops before it was stable) is one more step of the failure streak: step n waits
// baseMs * 2^(n-1), capped at maxMs, picked at random between half the step and the full step.
// Only a stable identified session clears the streak, so a peer that accepts and drops at once
// still backs off. Not thread-safe; the owner locks.
#pragma once
#include <cstdint>
#include <random>

namespace takaro {

struct ReconnectParams {
    int64_t fastMinMs = 500;   // first retry after a stable session: fastMinMs + [0, fastJitterMs]
    int64_t fastJitterMs = 750;
    int64_t baseMs = 2000;     // failure streak step 1 (TAKARO_RECONNECT_BASE_MS)
    int64_t maxMs = 60000;     // failure streak cap (TAKARO_RECONNECT_MAX_MS)
    int64_t stableMs = 10000;  // identified this long = a stable session
};

class ReconnectPolicy {
public:
    using Params = ReconnectParams;
    struct Delay {
        int64_t ms = 0;
        bool fast = false;    // fast retry after a stable session
        unsigned streak = 0;  // failure streak length (0 when fast)
    };
    // The operator's base/max; the fast window never exceeds the base (a 500 ms test base keeps
    // its fast retry under 500 ms + jitter of at most the base).
    static Params FromConfig(unsigned baseMs, unsigned maxMs);

    explicit ReconnectPolicy(Params p = Params(), uint64_t seed = 0);
    void OnIdentified(int64_t nowMs) { identifiedAtMs_ = nowMs; }
    // The current attempt or connection ended at nowMs: how long to wait before the next dial.
    Delay OnEnded(int64_t nowMs);
    // New settings: dial at once, a fresh streak.
    void Reset() {
        streak_ = 0;
        identifiedAtMs_ = -1;
    }
    unsigned Streak() const { return streak_; }
    const Params& params() const { return p_; }

private:
    int64_t Uniform(int64_t lo, int64_t hi);  // inclusive
    Params p_;
    std::mt19937_64 rng_;
    unsigned streak_ = 0;
    int64_t identifiedAtMs_ = -1;
};

}  // namespace takaro
