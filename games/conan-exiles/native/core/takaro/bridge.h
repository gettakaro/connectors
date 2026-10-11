// The Takaro side of the native connector: identify, request routing to the game, the durable event
// outbox and its delivery, and the health snapshot. Ported from the Enshrouded native connector
// (games/enshrouded/mod/src/native/bridge.h), behaviour checked against VEIN's native_bridge.cpp.
//
// Threads:
//  - the bridge thread owns all protocol state and the Store; it drains transport notices in batches;
//  - N action workers run Game::Execute; they may wait on the game thread, never touch the socket;
//  - the transport's thread only enqueues notices (OnNotice never blocks).
// A request whose action outlives actionTimeoutMs is answered with an error; its late result is dropped.
#pragma once
#include "common.h"
#include "takaro/config.h"
#include "takaro/config_watch.h"
#include "takaro/game.h"
#include "takaro/outbox.h"
#include "takaro/transport.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace takaro {

struct BridgeOptions {
    Config config;
    Game* game = nullptr;
    ITransport* transport = nullptr;
    Store* store = nullptr;
    std::function<int64_t()> steadyMs;  // monotonic; default steady_clock
    int64_t eventPollMs = 100;
    std::string healthFile;  // when set, the health snapshot is also written here every few seconds
    // When set, takaro.json is re-read while running (bridge thread) and the console banners shown.
    ConfigWatcher* watcher = nullptr;
};

class Bridge {
public:
    static constexpr size_t kNoticeBatch = 512;
    static constexpr size_t kMaxNotices = 4096;
    static constexpr size_t kMaxNoticeBytes = 16u << 20;
    static constexpr size_t kMaxPendingActions = 128;
    static constexpr size_t kMaxRequestIdBytes = 128;

    explicit Bridge(BridgeOptions o);
    ~Bridge();
    bool Start();  // loads the Store, starts the workers and the transport
    void Stop();
    bool OnNotice(Notice&& n);
    std::string HealthJson() const;  // cached snapshot; never waits on bridge work

private:
    struct Job {
        uint64_t id = 0;
        std::string requestId, action;
        JsonValue args;
        uint64_t epoch = 0;
        int64_t deadlineMs = 0, enqueuedMs = 0;
        std::atomic<int> state{0};  // 0 queued, 1 running, 2 done, 3 cancelled
        bool responded = false;     // bridge thread only
        ActionResult outcome;       // written by the worker before state=2
    };
    enum { kQueued = 0, kRunning = 1, kDone = 2, kCancelled = 3 };

    void Loop();
    void WorkerLoop();
    void HandleNotice(Notice& n);
    void HandleFrame(uint64_t epoch, const std::string& text);
    void HandleRequest(uint64_t epoch, const JsonValue& msg);
    void Complete(const std::shared_ptr<Job>& job);
    void CheckDeadlines(int64_t now);
    void SendResponse(uint64_t epoch, FrameKind kind, const std::string& frame);
    void PollEvents();
    void DeliverEvents();
    void FlushState(int64_t now, bool force);
    void Publish(int64_t now);
    void RememberId(const std::string& id);
    void ApplyConfig(Config next, int64_t now);
    void OnRefused(const std::string& why, bool nameTaken);

    BridgeOptions o_;
    std::thread thread_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> started_{false};

    mutable std::mutex mu_;
    std::condition_variable cv_, jobCv_;
    std::deque<Notice> notices_;
    size_t noticeBytes_ = 0;
    std::deque<std::shared_ptr<Job>> jobQueue_;
    std::deque<std::shared_ptr<Job>> completions_;

    // bridge-thread state
    uint64_t epoch_ = 0;
    bool open_ = false, identified_ = false;
    std::string gameServerId_;
    Config identifyConfig_;  // the settings the current epoch's identify was sent with
    uint64_t lastQueuedId_ = 0;
    std::map<uint64_t, std::shared_ptr<Job>> inflight_;
    std::set<std::string> pendingIds_;
    std::deque<std::string> recentIds_;
    std::set<std::string> recentIdSet_;
    uint64_t nextJobId_ = 1;
    bool outboxDirty_ = false;
    int64_t lastOutboxSave_ = 0, nextPoll_ = 0, nextPublish_ = 0, nextHealthFile_ = 0;
    std::string storeNotes_;

    uint64_t requests_ = 0, responses_ = 0, errorResponses_ = 0, duplicates_ = 0, malformed_ = 0, overloads_ = 0,
             timeouts_ = 0, lateResults_ = 0, droppedResponses_ = 0, identifyErrors_ = 0, identifies_ = 0,
             opens_ = 0, closes_ = 0, eventsAdmitted_ = 0, eventsQueued_ = 0, eventsRejected_ = 0,
             noticeOverflows_ = 0;
    int64_t maxRequestLatencyMs_ = 0;
    std::string lastError_, lastIdentifyError_, lastAction_;

    mutable std::mutex healthMu_;
    std::string health_ = "{}";
};

}  // namespace takaro
