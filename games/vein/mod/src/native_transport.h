#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// All payloads cross worker boundaries by value. The transport owns libwebsockets and
// invokes the notice sink on its service thread; the sink must only enqueue the notice.
namespace NativeTransport {
using Frame = std::shared_ptr<const std::string>;
enum class Kind { Control, CriticalResponse, Response, Event };
enum class QueueStatus { Accepted, Disconnected, StaleEpoch, Full, TooLarge };
struct Ticket { uint64_t epoch = 0, writeId = 0; };
struct Send {
    Kind kind;
    Frame frame;
    uint64_t expectedEpoch = 0;
    uint64_t outboxId = 0;
    bool pingAfterWrite = false;
};
struct QueueResult {
    QueueStatus status = QueueStatus::Disconnected;
    Ticket ticket;
    explicit operator bool() const { return status == QueueStatus::Accepted; }
};
enum class NoticeType { Open, Closed, Frame, Written, Confirmed, Error };
struct Notice {
    NoticeType type;
    uint64_t epoch = 0;
    uint64_t eventSeq = 0; // Written/Confirmed high watermark within this epoch.
    std::string text;
    Ticket ticket;
    uint64_t outboxId = 0;
    bool pingAfterWrite = false;
};
struct Config {
    std::string url;
    std::string caFile;
    std::string gameHttpUrl = "http://127.0.0.1:8080";
    unsigned reconnectBaseMs = 2000;
    unsigned reconnectMaxMs = 60000;
};
struct Stats {
    bool connected = false;
    uint64_t epoch = 0;
    size_t outboundMessages = 0;
    size_t outboundBytes = 0;
    size_t transientBytes = 0;
    uint64_t rejected = 0;
    uint64_t prunedEvents = 0;
    std::string lastError;
};
// false on a Frame causes the service thread to close the socket and replay.
using NoticeSink = std::function<bool(Notice)>;
bool Start(Config config, NoticeSink sink);
void Stop();
QueueResult Queue(Send send);
// Remove events evicted from the durable outbox. The service thread also checks
// this floor immediately before a frame write.
size_t PruneEventsBefore(uint64_t minOutboxId, uint64_t expectedEpoch);
// Wakes the LWS service thread. It owns the socket and emits a 1013 close.
bool RequestClose(uint64_t expectedEpoch, uint16_t code, std::string reason);
// Nonblocking; false means caller still owns the message and may retry or reject it.
// expectedEpoch prevents a late action result from crossing a reconnect. Zero
// is reserved for the identify frame queued immediately after Open.
bool Queue(Kind kind, std::string text, uint64_t eventSeq = 0, uint64_t expectedEpoch = 0);
Stats Snapshot();
// Action-worker-only read of VEIN's built-in read-only API. The LWS service
// thread owns the HTTP socket; the caller waits at most three seconds for an
// owned result. One request may be pending, and the body is capped at 1 MiB.
// Returns the raw HTTP 200 body for behavior-layer shape normalization.
std::optional<std::string> FetchGamePlayers();
} // namespace NativeTransport
