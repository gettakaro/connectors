// The process-wide link between the Lua protocol code and the WinHTTP transport. Thread-safe: Lua calls
// it from UE4SS's async/game threads, the transport's threads push notices into a bounded inbox.
// It knows nothing about the Takaro protocol beyond frame kinds; identify, requestId and args live in Lua.
#pragma once
#include "transport_winhttp.h"

#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace native {

struct InboxItem {
    std::string kind;  // "open" | "frame" | "closed" | "error" | "confirmed"
    uint64_t epoch = 0;
    std::string text;
};

class Bridge {
public:
    static Bridge& Get();
    // Starts the transport, or retargets it when it already runs. Returns false on an unusable URL.
    bool Connect(const std::string& url, const std::string& caFile);
    void Disconnect();  // stops the transport and joins its threads
    // Detaches the transport at once (a following Connect starts a fresh one) and stops it on a helper thread.
    void DisconnectAsync();
    // kind: control | critical | response | event. epoch 0 means the current connection.
    // *outboxId receives the event's outbox id (events only); a later "confirmed" notice with a watermark
    // >= that id means Takaro received it (its pong for a later ping arrived).
    QueueStatus Send(const std::string& text, const std::string& kind, uint64_t epoch, uint64_t* outboxId = nullptr);
    bool Poll(InboxItem& out);
    void Close(uint64_t epoch, const std::string& reason);
    void MarkIdentified(uint64_t epoch);
    std::string StatusJson();

private:
    // Transport sink: only enqueues. Notices of an older transport (gen != gen_) are dropped. Epochs handed to
    // Lua are (gen << 32 | transport epoch), so they never repeat across transports.
    bool Push(uint64_t gen, Notice&& n);

    std::mutex mu_;  // guards inbox_, inboxBytes_, dropped_, currentEpoch_
    std::deque<InboxItem> inbox_;
    size_t inboxBytes_ = 0;
    uint64_t dropped_ = 0, currentEpoch_ = 0, outboxSeq_ = 0, gen_ = 0;
    std::mutex lifeMu_;  // guards t_ lifetime
    std::unique_ptr<WinHttpTransport> t_;
    std::string url_;
};

constexpr size_t kInboxMaxItems = 4096;
constexpr size_t kInboxMaxBytes = 32u << 20;

}  // namespace native
