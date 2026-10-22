// Engine glue of the #203 research probe. Inert unless TAKARO_COMMAND_PROBE=1 (see command_probe.h).
#include "command_probe.h"

#include "actions.h"
#include "common.h"
#include "gamethread.h"

#include <chrono>
#include <memory>
#include <thread>

namespace CommandProbe {
namespace {
Config g_cfg;
Pending g_pending;

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
    uint64_t id = g_pending.Begin(NowMs(), g_cfg.deadlineMs);
    std::string puid = senderPuid;
    if (!id) {
        // Recognised Takaro input never fails open into public chat, even when we are saturated.
        PluginLog("probe: taken but in-flight table full; suppressed, answering busy");
        std::thread([puid] { Answer(0, Outcome::Unavailable, puid, "busy"); }).detach();
        return true;
    }
    StubAnswer a = Stub(g_cfg, c.mode);
    PluginLog("probe: #%llu TAKEN prefix=%c mode=%s sender=%s; original Server_SendChatMessage skipped",
              (unsigned long long)id, c.prefix, c.mode.c_str(), puid.c_str());
    uint32_t deadline = g_cfg.deadlineMs;
    std::thread([id, a, puid] {
        std::this_thread::sleep_for(std::chrono::milliseconds(a.delayMs));
        if (g_pending.Settle(id)) Answer(id, a.outcome, puid, "stub");
        else PluginLog("probe: #%llu late stub answer (%s) dropped; not replayed", (unsigned long long)id,
                       OutcomeName(a.outcome));
    }).detach();
    std::thread([id, deadline, puid] {
        std::this_thread::sleep_for(std::chrono::milliseconds(deadline));
        if (g_pending.Settle(id)) Answer(id, Outcome::Timeout, puid, "deadline");
    }).detach();
    return true;
}

}  // namespace CommandProbe
