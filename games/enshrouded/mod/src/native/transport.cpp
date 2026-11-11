#include "native/transport.h"

#include <algorithm>
#include <chrono>

namespace native {

const char* QueueStatusName(QueueStatus s) {
    switch (s) {
        case QueueStatus::Accepted: return "accepted";
        case QueueStatus::Disconnected: return "disconnected";
        case QueueStatus::StaleEpoch: return "stale-epoch";
        case QueueStatus::Full: return "full";
        case QueueStatus::TooLarge: return "too-large";
    }
    return "?";
}

FrameQueues::Limit FrameQueues::LimitFor(FrameKind k) {
    switch (k) {
        case FrameKind::Control: return {64, 1u << 20};
        case FrameKind::CriticalResponse: return {256, 1u << 20};
        case FrameKind::Response: return {256, 32u << 20};
        case FrameKind::Event: return {1024, 8u << 20};
    }
    return {0, 0};
}

QueueStatus FrameQueues::Push(OutFrame f) {
    if (!f.text || f.text->size() > kMaxOutboundFrame) {
        rejected_++;
        return QueueStatus::TooLarge;
    }
    int i = (int)f.kind;
    Limit lim = LimitFor(f.kind);
    if (q_[i].size() >= lim.frames || bytes_[i] + f.text->size() > lim.bytes) {
        rejected_++;
        return QueueStatus::Full;
    }
    bytes_[i] += f.text->size();
    q_[i].push_back(std::move(f));
    return QueueStatus::Accepted;
}

bool FrameQueues::Pop(OutFrame& out) {
    for (int i = 0; i < 4; i++) {
        if (q_[i].empty()) continue;
        out = std::move(q_[i].front());
        q_[i].pop_front();
        bytes_[i] -= out.text->size();
        return true;
    }
    return false;
}

void FrameQueues::Clear() {
    for (int i = 0; i < 4; i++) {
        q_[i].clear();
        bytes_[i] = 0;
    }
}

size_t FrameQueues::TotalFrames() const {
    size_t n = 0;
    for (auto& q : q_) n += q.size();
    return n;
}

size_t FrameQueues::TotalBytes() const { return bytes_[0] + bytes_[1] + bytes_[2] + bytes_[3]; }

ReconnectPolicy::ReconnectPolicy(Params p, uint64_t seed) : p_(p) {
    if (p_.maxMs < p_.baseMs) p_.maxMs = p_.baseMs;
    if (!seed)
        seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() ^
               (uint64_t)std::chrono::system_clock::now().time_since_epoch().count() ^ (uint64_t)(uintptr_t)this;
    rng_ = seed ? seed : 0x9E3779B97F4A7C15ull;
}

unsigned ReconnectPolicy::Rand(unsigned maxInclusive) {
    // xorshift64*: jitter only, no need for more
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    uint64_t r = rng_ * 0x2545F4914F6CDD1Dull;
    return (unsigned)((r >> 11) % ((uint64_t)maxInclusive + 1));
}

void ReconnectPolicy::OnIdentified(int64_t nowMs) { identifiedAtMs_ = nowMs; }

void ReconnectPolicy::Reset() {
    streak_ = 0;
    identifiedAtMs_ = -1;
}

unsigned ReconnectPolicy::NextDelayMs(int64_t nowMs, bool* established) {
    bool stable = identifiedAtMs_ >= 0 && nowMs - identifiedAtMs_ >= p_.stableMs;
    identifiedAtMs_ = -1;
    if (established) *established = stable;
    if (stable) {
        streak_ = 0;
        return p_.fastMinMs + Rand(p_.fastJitterMs);
    }
    streak_++;
    uint64_t d = (uint64_t)p_.baseMs << std::min(streak_ - 1, 20u);
    unsigned capped = (unsigned)std::min<uint64_t>(d, p_.maxMs);
    return capped - Rand(capped / 2);
}

void Heartbeat::Reset(int64_t nowMs) {
    pings_.clear();
    written_ = confirmed_ = pingsSent_ = pongs_ = 0;
    lastPingMs_ = lastInboundMs_ = nowMs;
}

void Heartbeat::OnEventWritten(uint64_t outboxId) {
    if (outboxId > written_) written_ = outboxId;
}

bool Heartbeat::PingDue(int64_t nowMs, bool eventQueueEmpty) const {
    if (pings_.size() >= p_.maxOutstanding) return false;
    int64_t since = nowMs - lastPingMs_;
    if (since >= p_.intervalMs) return true;
    // A burst of events just finished: ping now so its pong confirms them without waiting for the interval.
    uint64_t covered = pings_.empty() ? confirmed_ : pings_.back();
    return eventQueueEmpty && written_ > covered && since >= p_.eagerMs;
}

void Heartbeat::OnPingSent(int64_t nowMs) {
    pings_.push_back(written_);
    lastPingMs_ = nowMs;
    pingsSent_++;
}

uint64_t Heartbeat::OnPong() {
    pongs_++;
    if (pings_.empty()) return 0;  // unsolicited pong: confirms nothing
    uint64_t w = pings_.front();
    pings_.pop_front();
    if (w <= confirmed_) return 0;
    confirmed_ = w;
    return w;
}

}  // namespace native
