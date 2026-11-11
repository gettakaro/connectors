// The Takaro Generic connection, free of any game or AsaApi type so it can be tested on its own.
// Threads: the transport's own threads only enqueue notices; one worker owns identify, request parsing,
// response serialising, the event window and acks. The game thread only calls NextCall, Complete and Emit,
// none of which block or touch the network.
#pragma once
#include "tj_json.hpp"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace takaro {

struct Config {
    std::string registrationToken;
    std::string identityToken;
    std::string serverName;
    std::string gameVersion;
    bool wireDebug = false;
    std::chrono::milliseconds callDeadline{9000};  // Takaro gives up after 10 s
    size_t maxPendingCalls = 64;
    size_t eventWindow = 64;
    size_t maxQueuedEvents = 8192;
};

// What the game thread returns for one action.
struct Outcome {
    tj::Value payload;
    std::string errorCode;  // empty = success; else unsupported|invalid_args|not_found|game_error|internal
    std::string errorMessage;
    static Outcome Ok(tj::Value v) { Outcome o; o.payload = std::move(v); return o; }
    static Outcome Fail(std::string code, std::string message) {
        Outcome o;
        o.errorCode = std::move(code);
        o.errorMessage = std::move(message);
        return o;
    }
};

struct GameCall {
    std::string requestId;
    std::string action;
    tj::Value args;  // always an object
    uint64_t epoch = 0;
};

// Takaro sends args as [], {}, an object or a JSON-encoded string; anything else counts as empty.
tj::Value NormalizeArgs(const tj::Value* raw);
// A player-bound action names the player as gameId or player.gameId. Empty when neither is a string.
std::string PlayerIdOf(const tj::Value& args);

class Client {
public:
    using LogFn = std::function<void(const std::string&)>;
    // Answers on the worker so it still works while the game thread is busy or the world is loading.
    using ReachabilityFn = std::function<tj::Value()>;
    // The identify capability manifest (only what works), and worker-side actions such as getGameIcon.
    using ManifestFn = std::function<std::vector<tj::Value>()>;
    using LocalActionFn = std::function<bool(const std::string& action, Outcome& out)>;

    Client(Config config, native::ITransport* transport, std::string streamId, LogFn log);
    ~Client();
    void SetReachability(ReachabilityFn fn) { reachability_ = std::move(fn); }
    void SetManifest(ManifestFn fn) { manifest_ = std::move(fn); }
    void SetLocalAction(LocalActionFn fn) { localAction_ = std::move(fn); }

    bool Start();
    void Stop();

    // Game thread. Calls already answered as timed out are never handed out, so an action
    // reported failed to Takaro cannot run afterwards.
    bool NextCall(GameCall& out);
    void Complete(const GameCall& call, Outcome outcome);
    void Emit(const std::string& type, tj::Value data);

    bool Identified() const { return identified_.load(); }
    int ProtocolVersion() const { return protocol_.load(); }
    std::string GameServerId();
    uint64_t DroppedEvents() const { return droppedEvents_.load(); }

    // For tests: attach to the transport without the worker thread, then drive it with PumpOnce.
    bool StartManual();
    void PumpOnce();

private:
    struct QueuedEvent {
        uint64_t seq = 0;
        std::shared_ptr<const std::string> v0, v1;
    };
    struct Finished {
        GameCall call;
        Outcome outcome;
    };

    bool Enqueue(native::Notice&& n);
    void Run();
    void HandleNotice(native::Notice& n);
    void HandleFrame(uint64_t epoch, const std::string& raw);
    void HandleRequest(uint64_t epoch, const tj::Value& message);
    void Respond(uint64_t epoch, const std::string& requestId, Outcome outcome, const std::string& action);
    void SendIdentify(uint64_t epoch);
    void ExpireCalls();
    void PumpEvents();
    void DropAcked(uint64_t upTo);
    std::string IdentifyFrame() const;

    Config config_;
    native::ITransport* transport_;
    std::string streamId_;
    LogFn log_;
    ReachabilityFn reachability_;
    ManifestFn manifest_;
    LocalActionFn localAction_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<native::Notice> notices_;
    std::deque<Finished> finished_;
    std::deque<GameCall> calls_;
    std::map<std::string, std::chrono::steady_clock::time_point> pending_;  // requestId -> deadline
    std::deque<QueuedEvent> events_;
    size_t eventsSent_ = 0;  // events_[0..eventsSent_) are written and await an ack
    uint64_t nextSeq_ = 1;
    bool stopping_ = false;

    // Worker-only.
    uint64_t epoch_ = 0;

    std::atomic<bool> identified_{false};
    std::atomic<int> protocol_{0};
    std::atomic<uint64_t> droppedEvents_{0};
    std::mutex idMu_;
    std::string gameServerId_;
    std::thread worker_;
};

}  // namespace takaro
