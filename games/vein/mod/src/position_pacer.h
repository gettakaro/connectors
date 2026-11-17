#pragma once
#include <chrono>
#include <cstdint>

namespace NativeBridge {
// Wait after receipt confirmation so transport stalls cannot compress position batches.
class PositionPacer {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    void Reset(Time now) { pending = 0; next = now + std::chrono::seconds(3); }
    bool Ready(Time now) const { return !pending && now >= next; }
    void Sent(uint64_t seq, Time now) { pending = seq; sent = now; }
    void Ack(uint64_t upTo, Time now) {
        if (pending && upTo >= pending) { pending = 0; next = now + std::chrono::seconds(3); }
    }
    bool TimedOut(Time now) const { return pending && now - sent >= std::chrono::seconds(30); }
private:
    uint64_t pending = 0;
    Time next{}, sent{};
};
}
