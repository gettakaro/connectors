#include "poller.h"

#include "common.h"
#include "http.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Poller {
namespace {
// Function-local and leaked on purpose: Start() runs from the library constructor, before other
// translation units are initialized, and the thread may outlive static destructors at exit.
struct State {
    std::thread* thread = nullptr;
    std::mutex lock;
    std::condition_variable wake;
    bool exited = false;
};
State& S() {
    static State* s = new State;
    return *s;
}
std::atomic<bool> g_stop{false};

void SleepMs(int ms) {
    std::unique_lock<std::mutex> lk(S().lock);
    S().wake.wait_for(lk, std::chrono::milliseconds(ms), [] { return g_stop.load(); });
}

void Loop(Config cfg, Executor chat) {
    const std::string pollPath = std::string("/mod/poll?source=") + kModSource;
    int backoff = cfg.intervalMs;
    bool connected = false;
    std::string lastError;
    while (!g_stop.load()) {
        FlushNativeLogs();
        HttpResult r = HttpRequest(cfg.url, "GET", pollPath, "", cfg.requestTimeoutMs);
        std::string err = !r.ok ? r.error : r.status != 200 ? "HTTP " + std::to_string(r.status) + " " + r.body : "";
        PollCommand cmd;
        if (err.empty() && !ParsePoll(r.body, cmd, err)) err = "bad poll body: " + err;
        if (!err.empty()) {
            if (connected || err != lastError)
                NativeLog("bridge poll failed (%s); retrying with backoff", err.c_str());
            connected = false;
            lastError = err;
            SleepMs(backoff);
            backoff = backoff * 2 > cfg.maxBackoffMs ? cfg.maxBackoffMs : backoff * 2;
            continue;
        }
        if (!connected) NativeLog("bridge connected: %s:%d", cfg.url.host.c_str(), cfg.url.port);
        connected = true;
        lastError.clear();
        backoff = cfg.intervalMs;
        if (!cmd.has) {
            SleepMs(cfg.intervalMs);
            continue;
        }
        ChatOutcome outcome = Execute(cmd, chat);
        NativeLog("command %s %s: %s%s", cmd.requestId.c_str(), cmd.action.c_str(),
                  outcome.success ? "ok" : "failed: ", outcome.error.c_str());
        // The bridge already dequeued the command, so a lost result means Takaro times out even
        // though the line may have been shown. Retry transport failures; a 4xx is final (the
        // bridge no longer knows the requestId, e.g. it timed out).
        const std::string result = ResultBody(cmd.requestId, outcome);
        for (int attempt = 1; attempt <= 3 && !g_stop.load(); attempt++) {
            HttpResult post = HttpRequest(cfg.url, "POST", "/mod/result", result, cfg.requestTimeoutMs);
            if (post.ok && post.status == 200) break;
            NativeLog("result for %s not accepted (%s, HTTP %d), attempt %d", cmd.requestId.c_str(),
                      post.ok ? post.body.c_str() : post.error.c_str(), post.status, attempt);
            if (post.ok && post.status >= 400 && post.status < 500) break;
            SleepMs(200 * attempt);
        }
        // Poll again at once: the bridge hands out one queued command per poll.
    }
    FlushNativeLogs();
    std::lock_guard<std::mutex> lk(S().lock);
    S().exited = true;
    S().wake.notify_all();
}
}  // namespace

ChatOutcome Execute(const PollCommand& cmd, const Executor& chat) {
    if (cmd.action != "sendMessage") {
        ChatOutcome o;
        o.error = "Unsupported action " + cmd.action + " in the Takaro Conan native library";
        return o;
    }
    return chat(cmd);
}

void Start(const Config& cfg, Executor chat) {
    g_stop = false;
    S().thread = new std::thread(Loop, cfg, std::move(chat));
}

void Stop() {
    {
        std::lock_guard<std::mutex> lk(S().lock);
        g_stop = true;
    }
    S().wake.notify_all();
    if (!S().thread) return;
    // Bounded: a request can be stuck in name resolution, and process exit must not wait on it.
    // The thread object is leaked either way, so an unjoined thread is never destroyed.
    std::unique_lock<std::mutex> lk(S().lock);
    if (S().wake.wait_for(lk, std::chrono::seconds(2), [] { return S().exited; })) {
        lk.unlock();
        S().thread->join();
    }
}

}  // namespace Poller
