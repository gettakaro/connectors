// Engine glue of the #203 research probe. Inert unless TAKARO_COMMAND_PROBE=1 (see command_probe.h).
#include "command_probe.h"

#include "actions.h"
#include "common.h"
#include "gamethread.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace CommandProbe {
namespace {
Config g_cfg;
Pending g_pending;
// Every reply worker counts here, so a burst can never start more than this many threads.
std::atomic<int> g_workers{0};
const int kMaxWorkers = 16;

// Starts fn on a detached worker within the cap; false when the cap is hit or the start fails.
template <typename F>
bool StartWorker(F fn) {
    if (g_workers.fetch_add(1) >= kMaxWorkers) {
        g_workers.fetch_sub(1);
        return false;
    }
    try {
        std::thread([fn] {
            fn();
            g_workers.fetch_sub(1);
        }).detach();
        return true;
    } catch (...) {
        g_workers.fetch_sub(1);
        return false;
    }
}

// Runs on a worker: never answer from inside the hooked ProcessEvent.
void Answer(uint64_t id, Outcome o, const std::string& puid, const char* via) {
    std::string text = ReplyText(o, id);
    // Shared, because a timed-out Run may still execute the job after we return.
    struct R { bool sent = false; std::string err; };
    auto r = std::make_shared<R>();
    bool ran = GameThread::Run([r, puid, text] { r->sent = Actions::ProbeWhisper(puid, text, r->err); }, 3000);
    bool ok = ran && r->sent;
    std::string err = !ran ? "game thread unavailable" : r->err;
    PluginLog("probe: #%llu settled %s via %s -> private reply to %s %s%s", (unsigned long long)id, OutcomeName(o),
              via, puid.c_str(), ok ? "delivered" : "FAILED: ", ok ? "" : err.c_str());
}
}  // namespace

void Init() {
    g_cfg = ParseConfig([](const char* k) { return ConfigValue(k, ""); });
    if (g_cfg.enabled)
        PluginLog("probe: ON (research #203) nonce=%s player=%s stub=%s deadline=%ums", g_cfg.nonce.c_str(),
                  g_cfg.player.c_str(), g_cfg.stubUp ? "up" : "down", g_cfg.deadlineMs);
    else if (!g_cfg.error.empty())
        PluginLog("probe: requested but OFF: %s", g_cfg.error.c_str());
}

bool Enabled() { return g_cfg.enabled; }

bool OnServerChat(const std::string& msg, const std::string& senderPuid) {
    if (!g_cfg.enabled) return false;
    Classified c = Classify(g_cfg, msg, senderPuid);
    if (c.match != Match::Probe) {
        if (!c.reason.empty())
            PluginLog("probe: passed through as game chat (%s) from %s: %s", c.reason.c_str(), senderPuid.c_str(),
                      msg.c_str());
        return false;
    }
    // From here on the line is Takaro's: whatever fails below, it never reaches public chat.
    uint64_t id = g_pending.Begin(NowMs(), g_cfg.deadlineMs);
    std::string puid = senderPuid;
    if (!id) {
        PluginLog("probe: taken but in-flight table full; suppressed, answering busy");
        if (!StartWorker([puid] { Answer(0, Outcome::Unavailable, puid, "busy"); }))
            PluginLog("probe: saturated; suppressed without a reply");
        return true;
    }
    StubAnswer a = Stub(g_cfg, c.mode);
    PluginLog("probe: #%llu TAKEN prefix=%c mode=%s sender=%s; original Server_SendChatMessage skipped",
              (unsigned long long)id, c.prefix, c.mode.c_str(), puid.c_str());
    uint32_t deadline = g_cfg.deadlineMs;
    bool stub = StartWorker([id, a, puid] {
        std::this_thread::sleep_for(std::chrono::milliseconds(a.delayMs));
        if (g_pending.Settle(id, NowMs(), false)) Answer(id, a.outcome, puid, "stub");
        else PluginLog("probe: #%llu late stub answer (%s) dropped; not replayed", (unsigned long long)id,
                       OutcomeName(a.outcome));
    });
    bool timer = StartWorker([id, deadline, puid] {
        std::this_thread::sleep_for(std::chrono::milliseconds(deadline));
        if (g_pending.Settle(id, NowMs(), true)) Answer(id, Outcome::Timeout, puid, "deadline");
    });
    if (!stub || !timer) {
        // Without both workers nobody may answer; settle it here so no late reply ever appears.
        g_pending.Settle(id, 0, true);
        PluginLog("probe: #%llu suppressed without a reply (no worker available)", (unsigned long long)id);
    }
    return true;
}

}  // namespace CommandProbe
