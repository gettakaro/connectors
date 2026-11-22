#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace takaro {

constexpr int64_t kPositionIntervalMs = 3000;

// Receipt-based pacing prevents writes delayed by the socket from arriving in a burst.
class PositionPacer {
public:
    void Reset(std::string stream, int64_t now) {
        stream_ = std::move(stream);
        nextSeq_ = 1;
        waitingSeq_ = 0;
        notBefore_ = now;
        queuedAt_ = 0;
    }
    const std::string& Stream() const { return stream_; }
    uint64_t NextSeq() const { return nextSeq_; }
    bool CanSend(int64_t now) const { return waitingSeq_ == 0 && now >= notBefore_; }
    void Sent(int64_t now) { waitingSeq_ = nextSeq_++; queuedAt_ = now; }
    bool Acknowledge(const std::string& stream, uint64_t seq, int64_t now) {
        if (stream != stream_ || !waitingSeq_ || seq != waitingSeq_) return false;
        waitingSeq_ = 0;
        notBefore_ = now + kPositionIntervalMs;
        return true;
    }
    bool TimedOut(int64_t now) const { return waitingSeq_ != 0 && now - queuedAt_ >= 30000; }

private:
    std::string stream_;
    uint64_t nextSeq_ = 1, waitingSeq_ = 0;
    int64_t notBefore_ = 0, queuedAt_ = 0;
};

}  // namespace takaro
