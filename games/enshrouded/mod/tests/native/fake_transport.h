// In-memory ITransport for bridge tests: the test plays the socket. Frames the bridge queues are "written"
// by Drain() in the same priority order the WinHTTP send thread uses; pings/pongs go through the real
// Heartbeat so confirmations follow the production rule.
#pragma once
#include "native/transport.h"
#include "testlib.h"

#include <atomic>
#include <mutex>

namespace t {

struct Written {
    native::FrameKind kind;
    std::string text;
    uint64_t outboxId = 0;
    uint64_t epoch = 0;
};

class LoopbackTransport : public native::ITransport {
public:
    bool Start(native::NoticeSink sink) override {
        std::lock_guard<std::mutex> g(mu);
        sink_ = std::move(sink);
        started = true;
        return true;
    }
    void Stop() override {
        std::lock_guard<std::mutex> g(mu);
        started = false;
    }
    native::QueueStatus Queue(native::OutFrame f) override {
        std::lock_guard<std::mutex> g(mu);
        if (!open) return native::QueueStatus::Disconnected;
        if (f.epoch != epoch) {
            stale++;
            return native::QueueStatus::StaleEpoch;
        }
        return queues.Push(std::move(f));
    }
    void RequestClose(uint64_t e, const std::string& reason) override {
        std::lock_guard<std::mutex> g(mu);
        closeRequests.push_back(std::to_string(e) + ":" + reason);
    }
    void MarkIdentified(uint64_t e) override {
        std::lock_guard<std::mutex> g(mu);
        if (e == epoch) identifiedEpoch = e;
    }
    std::string StatsJson() override { return "{\"fake\":true}"; }

    // ---- the test's side of the socket ----
    uint64_t Open() {
        native::NoticeSink s;
        uint64_t e;
        {
            std::lock_guard<std::mutex> g(mu);
            e = ++epoch;
            open = true;
            queues.Clear();
            hb.Reset(SteadyMs());
            s = sink_;
        }
        s({native::NoticeType::Open, e, "", 0});
        return e;
    }
    void Close(const std::string& why = "test close") {
        native::NoticeSink s;
        uint64_t e;
        {
            std::lock_guard<std::mutex> g(mu);
            open = false;
            queues.Clear();
            e = epoch;
            s = sink_;
        }
        s({native::NoticeType::Closed, e, why, 0});
    }
    bool Inject(const std::string& text) {
        native::NoticeSink s;
        uint64_t e;
        {
            std::lock_guard<std::mutex> g(mu);
            e = epoch;
            s = sink_;
        }
        return s({native::NoticeType::Frame, e, text, 0});
    }
    // Writes up to `max` queued frames (priority order) to the fake wire and returns them.
    std::vector<Written> Drain(size_t max = (size_t)-1) {
        std::vector<Written> out;
        std::lock_guard<std::mutex> g(mu);
        native::OutFrame f;
        while (out.size() < max && queues.Pop(f)) {
            if (f.kind == native::FrameKind::Event) hb.OnEventWritten(f.outboxId);
            out.push_back({f.kind, *f.text, f.outboxId, f.epoch});
            wire.push_back(out.back());
        }
        return out;
    }
    // A ping reaches the wire now (after everything drained so far).
    void Ping() {
        std::lock_guard<std::mutex> g(mu);
        hb.OnPingSent(SteadyMs());
    }
    // Takaro's pong for the oldest outstanding ping arrives.
    void Pong() {
        native::NoticeSink s;
        uint64_t confirmed, e;
        {
            std::lock_guard<std::mutex> g(mu);
            confirmed = hb.OnPong();
            e = epoch;
            s = sink_;
        }
        if (confirmed) s({native::NoticeType::Confirmed, e, "", confirmed});
    }
    size_t Queued() {
        std::lock_guard<std::mutex> g(mu);
        return queues.TotalFrames();
    }
    // All frames written so far that match `type` (and requestId when given).
    std::vector<JsonValue> WireFrames(const std::string& type, const std::string& requestId = "") {
        std::lock_guard<std::mutex> g(mu);
        std::vector<JsonValue> out;
        for (auto& w : wire) {
            JsonValue v;
            if (!native::ParseJson(w.text, v)) continue;
            const JsonValue* ty = v.get("type");
            if (!ty || ty->str != type) continue;
            if (!requestId.empty() && (!v.get("requestId") || v.get("requestId")->str != requestId)) continue;
            out.push_back(v);
        }
        return out;
    }
    std::vector<std::string> CloseRequests() {
        std::lock_guard<std::mutex> g(mu);
        return closeRequests;
    }
    uint64_t Epoch() {
        std::lock_guard<std::mutex> g(mu);
        return epoch;
    }

    std::mutex mu;
    native::NoticeSink sink_;
    bool started = false, open = false;
    uint64_t epoch = 0, stale = 0;
    std::atomic<uint64_t> identifiedEpoch{0};
    native::FrameQueues queues;
    native::Heartbeat hb;
    std::vector<Written> wire;
    std::vector<std::string> closeRequests;
};

}  // namespace t
