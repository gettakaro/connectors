// The Takaro side of the native connector: identify, request routing, event outbox delivery,
// online reconciliation, timed bans and the log-tail fallback. Replaces the former TypeScript sidecar
// (sidecar/src/bridge.ts, index.ts, takaro/client.ts, enshrouded/eventPoller.ts).
//
// Threads:
//  - the bridge thread owns all protocol state and the Store; it drains transport notices in batches of
//    512, so neither a replay backlog nor a burst of confirmations can starve requests;
//  - N action workers run ExecuteAction against the plugin (GameApi). They may wait on the game's own
//    queues; they never touch the socket or the Store;
//  - the transport's threads only enqueue notices (OnNotice never blocks).
// A request whose action outlives actionTimeoutMs is answered with an error; its late result is dropped,
// but state changes it made in the game (a ban) are still recorded.
#pragma once
#include "common.h"
#include "native/adapter.h"
#include "native/config.h"
#include "native/game_api.h"
#include "native/logtail.h"
#include "native/persistence.h"
#include "native/transport.h"

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

namespace native {

struct BridgeOptions {
    NativeConfig config;
    GameApi* game = nullptr;
    ITransport* transport = nullptr;
    Store* store = nullptr;
    std::function<int64_t()> steadyMs;  // monotonic
    std::function<int64_t()> wallMs;    // Unix epoch
    int64_t reconcileIntervalMs = 30000;
    int64_t healthIntervalMs = 15000;
    int64_t banCheckIntervalMs = 5000;
    // Called on the bridge thread; they must not block.
    std::function<void(const LiveSettings& accepted)> onIdentified;
    // summary: "<name>: <message> (HTTP <status>)" built from the error's name, message and status only
    // (Takaro's error can carry its internal request, tokens included). httpStatus 0 when unknown.
    std::function<void(const std::string& summary, int httpStatus)> onIdentifyRejected;
};

// The safe summary of a Takaro identify error, and its HTTP status (0 when it names none).
std::string IdentifyErrorSummary(const JsonValue* error, int& httpStatus);

class Bridge {
public:
    static constexpr size_t kNoticeBatch = 512;
    static constexpr size_t kMaxNotices = 4096;
    static constexpr size_t kMaxNoticeBytes = 16u << 20;
    static constexpr size_t kMaxPendingActions = 128;
    static constexpr size_t kMaxRequestIdBytes = 128;
    static constexpr int64_t kLocationFallbackMs = 60000;

    explicit Bridge(BridgeOptions o);
    ~Bridge();
    // Loads the Store (recovering ban intents), starts the workers and the transport.
    bool Start();
    void Stop();
    bool OnNotice(Notice&& n);
    // New connection settings from any thread: the bridge thread applies them, then drops the connection
    // and reconnects at once (or stays idle when !connect).
    void Reconfigure(const LiveSettings& s, bool connect);
    std::string HealthJson() const;  // cached snapshot; safe from any thread, never waits on bridge work

    // test helpers (bridge-thread state; call only while the bridge is stopped or from its callbacks)
    void NoteConnectionEvent(const std::string& type, const JsonValue& data, int64_t nowMs);
    bool EventLocationFallback(const JsonValue& args, int64_t nowMs, JsonValue& out);

private:
    struct Job {
        uint64_t id = 0;
        std::string requestId, action;
        JsonValue args;
        uint64_t epoch = 0;
        int64_t deadlineMs = 0;
        bool expiry = false;  // internal timed-ban expiry
        std::string expiryGameId;
        std::string intentId;
        std::shared_ptr<const ActionView> view;
        std::atomic<int> state{0};  // 0 queued, 1 running, 2 done, 3 cancelled
        bool responded = false;     // bridge thread only
        ActionOutcome outcome;      // written by the worker before state=2
        int64_t enqueuedMs = 0, startedMs = 0, finishedMs = 0;
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
    void PollEvents(int64_t now);
    void DeliverEvents(int64_t now);
    void AdmitEvent(const std::string& type, const JsonValue& data, const SourceCursor& source, int64_t now);
    void ObserveConnection(const std::string& type, const JsonValue& data, int64_t now);
    void Reconcile(bool strict, int64_t now);
    void RefreshHealth(int64_t now);
    void PollTail(int64_t now);
    void CheckTimedBans(int64_t now);
    void RecoverBanIntents();
    void FlushState(int64_t now, bool force);
    void Publish();
    std::shared_ptr<const ActionView> View();
    void RememberPosition(const JsonValue& args, const JsonValue& payload);
    std::string Redact(std::string text) const;
    void ApplyPendingSettings();

    BridgeOptions o_;
    std::thread thread_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> started_{false};

    // shared with transport + workers
    mutable std::mutex mu_;
    std::condition_variable cv_, jobCv_;
    std::deque<Notice> notices_;
    size_t noticeBytes_ = 0;
    std::deque<std::shared_ptr<Job>> jobQueue_;
    std::deque<std::shared_ptr<Job>> completions_;
    bool settingsPending_ = false, pendingConnect_ = false;
    LiveSettings pendingSettings_;

    // bridge-thread state
    uint64_t epoch_ = 0;
    bool open_ = false, identified_ = false;
    std::string gameServerId_, clientId_;
    uint64_t lastQueuedId_ = 0;
    std::map<uint64_t, std::shared_ptr<Job>> inflight_;
    std::set<std::string> pendingIds_;
    std::deque<std::string> recentIds_;
    std::set<std::string> recentIdSet_;
    uint64_t nextJobId_ = 1;
    std::shared_ptr<const ActionView> view_;
    bool viewDirty_ = true;
    std::map<std::string, int64_t> windows_;
    std::map<std::string, JsonValue> lastPositions_;
    std::map<std::string, int> missingStrikes_;
    bool reconcilePending_ = true;
    std::set<std::string> expiryInFlight_;
    LogTailer tailer_;
    bool tailActive_ = false;
    bool healthKnown_ = false;
    std::string pluginStatus_;
    bool outboxDirty_ = false, onlineDirty_ = false, knownDirty_ = false;
    int64_t lastOutboxSave_ = 0, lastKnownSave_ = 0;
    int64_t nextPoll_ = 0, nextReconcile_ = 0, nextHealth_ = 0, nextBanCheck_ = 0, nextPublish_ = 0;
    std::string lastConnectorState_;

    // counters (bridge thread; published in HealthJson)
    uint64_t requests_ = 0, responses_ = 0, errorResponses_ = 0, duplicates_ = 0, malformed_ = 0, overloads_ = 0,
             timeouts_ = 0, lateResults_ = 0, droppedResponses_ = 0, oversizeIds_ = 0, identifyErrors_ = 0,
             eventsAdmitted_ = 0, eventsQueued_ = 0, eventsMalformed_ = 0, ringGaps_ = 0, reconciled_ = 0,
             tailEvents_ = 0, expiredBans_ = 0, noticeOverflows_ = 0;
    int64_t maxRequestLatencyMs_ = 0;
    std::string lastError_, lastIdentifyError_, lastRequestAction_;
    std::vector<std::string> oldSecrets_;  // tokens used before a Reconfigure, still redacted

    mutable std::mutex healthMu_;
    std::string health_ = "{}";
};

}  // namespace native
