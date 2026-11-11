// Transport contract between the Takaro bridge and the WebSocket implementation.
//
// Every connection is an epoch (1, 2, ...). A frame queued for an older epoch is refused, so a late
// action result can never cross a reconnect. Queue() never blocks. The transport owns heartbeat and
// delivery confirmation: it sends Takaro's application ping ({"type":"ping"}, answered in order with
// {"type":"pong"}) and, when a pong arrives, reports Confirmed with the highest event outbox id it had
// written before that ping. An event is therefore confirmed only by the pong of a LATER ping on the
// same connection. Notices reach the bridge through a sink that must only enqueue.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace native {

enum class FrameKind { Control = 0, CriticalResponse = 1, Response = 2, Event = 3 };
enum class QueueStatus { Accepted, Disconnected, StaleEpoch, Full, TooLarge };
const char* QueueStatusName(QueueStatus s);

struct OutFrame {
    FrameKind kind = FrameKind::Control;
    std::shared_ptr<const std::string> text;
    uint64_t epoch = 0;     // must equal the open connection's epoch
    uint64_t outboxId = 0;  // events only
};

enum class NoticeType { Open, Frame, Confirmed, Closed, Error };
struct Notice {
    NoticeType type = NoticeType::Error;
    uint64_t epoch = 0;
    std::string text;       // Frame: the message; Closed/Error: reason
    uint64_t outboxId = 0;  // Confirmed: every event with id <= this was received by Takaro
};
// Returns false when the bridge cannot take more; the transport then drops the connection.
using NoticeSink = std::function<bool(Notice&&)>;

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool Start(NoticeSink sink) = 0;
    virtual void Stop() = 0;
    virtual QueueStatus Queue(OutFrame frame) = 0;
    virtual void RequestClose(uint64_t epoch, const std::string& reason) = 0;
    // Identify succeeded on this epoch: once it has stayed up for ReconnectPolicy's stable window,
    // its loss is retried fast and ends any failure streak.
    virtual void MarkIdentified(uint64_t epoch) = 0;
    // New connection settings: drop the current connection and, when `connect`, open a new one to
    // `url` at once (no backoff); otherwise stay idle until the next Retarget.
    virtual void Retarget(const std::string& url, bool connect) = 0;
    virtual std::string StatsJson() = 0;
};

// Limits shared by every transport (and asserted by the host tests).
constexpr size_t kMaxInboundBytes = 1u << 20;   // one Takaro message
constexpr size_t kMaxOutboundFrame = 8u << 20;  // one outbound message (listItems is ~0.6 MiB)

// Bounded per-kind FIFO queues with strict priority Control > CriticalResponse > Response > Event.
// Not thread-safe; the owner locks.
class FrameQueues {
public:
    struct Limit {
        size_t frames, bytes;
    };
    static Limit LimitFor(FrameKind k);
    QueueStatus Push(OutFrame f);
    bool Pop(OutFrame& out);
    void Clear();
    size_t Frames(FrameKind k) const { return q_[(int)k].size(); }
    size_t Bytes(FrameKind k) const { return bytes_[(int)k]; }
    size_t TotalFrames() const;
    size_t TotalBytes() const;
    uint64_t Rejected() const { return rejected_; }

private:
    std::deque<OutFrame> q_[4];
    size_t bytes_[4] = {0, 0, 0, 0};
    uint64_t rejected_ = 0;
};

// Application heartbeat and delivery confirmation, shared by every transport. Takaro answers each
// {"type":"ping"} with one {"type":"pong"} in order, so pings form a FIFO: each remembers the highest
// event outbox id written before it, and its pong confirms exactly that. A lost pong only delays
// confirmation (a later pong pops an older entry); it can never confirm an event written after the ping.
// Not thread-safe; the owner locks.
// When to reconnect after a connection ends (gettakaro/connectors#858). A connection that was identified
// and then stayed up for stableMs is "established": losing it is retried after fastMinMs + [0, fastJitterMs]
// (well under 2 s, jittered so a Takaro deploy does not bring every server back at the same instant) and the
// failure streak resets. Any other end (connect failed, closed before identify, dropped within stableMs) adds
// to the streak: base * 2^(streak-1), capped at maxMs, with equal jitter (a random point in [d/2, d]).
// Not thread-safe; the owner locks.
class ReconnectPolicy {
public:
    struct Params {
        unsigned baseMs = 2000, maxMs = 60000;      // TAKARO_RECONNECT_BASE_MS / _MAX_MS
        unsigned fastMinMs = 250, fastJitterMs = 500;
        int64_t stableMs = 10000;                    // like 7D2D's CONNECTION_STABLE_SECONDS
    };
    ReconnectPolicy() : ReconnectPolicy(Params{}, 0) {}
    // seed 0 = seed from the clock and the object address
    ReconnectPolicy(Params p, uint64_t seed);
    void OnIdentified(int64_t nowMs);  // identify accepted on the current connection
    void Reset();                      // new settings: the streak no longer applies
    // Call once per ended connection or failed attempt. Returns the delay before the next attempt and
    // whether the ended connection counted as established.
    unsigned NextDelayMs(int64_t nowMs, bool* established = nullptr);
    unsigned Streak() const { return streak_; }

private:
    unsigned Rand(unsigned maxInclusive);
    Params p_;
    uint64_t rng_;
    unsigned streak_ = 0;
    int64_t identifiedAtMs_ = -1;
};

class Heartbeat {
public:
    struct Params {
        int64_t intervalMs = 5000;  // regular ping
        int64_t idleMs = 20000;     // no inbound frame for this long = dead link
        int64_t eagerMs = 1000;     // min gap for the extra ping that confirms a finished event burst
        size_t maxOutstanding = 8;
    };
    Heartbeat() = default;
    explicit Heartbeat(Params p) : p_(p) {}
    void Reset(int64_t nowMs);
    void OnInbound(int64_t nowMs) { lastInboundMs_ = nowMs; }
    void OnEventWritten(uint64_t outboxId);
    bool PingDue(int64_t nowMs, bool eventQueueEmpty) const;
    void OnPingSent(int64_t nowMs);  // call when the ping is handed to the socket
    uint64_t OnPong();               // watermark newly confirmed by this pong (0: nothing new)
    bool Dead(int64_t nowMs) const { return nowMs - lastInboundMs_ > p_.idleMs; }
    size_t Outstanding() const { return pings_.size(); }
    uint64_t Written() const { return written_; }
    uint64_t Confirmed() const { return confirmed_; }
    uint64_t PingsSent() const { return pingsSent_; }
    uint64_t PongsReceived() const { return pongs_; }

private:
    Params p_;
    std::deque<uint64_t> pings_;  // watermark per outstanding ping
    uint64_t written_ = 0, confirmed_ = 0, pingsSent_ = 0, pongs_ = 0;
    int64_t lastPingMs_ = 0, lastInboundMs_ = 0;
};

}  // namespace native
