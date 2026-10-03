#include "conan/chat.h"

#include "common.h"
#include "conan/text.h"
#include "gamethread.h"
#include "ue/ue.h"


#include <vector>

namespace conan {
namespace {
constexpr int kJobTimeoutMs = 2000;
constexpr int kMaxDiscoverySteps = 1024;  // 16384 objects per step

ChatOutcome Fail(const std::string& error) {
    ChatOutcome o;
    o.error = error;
    return o;
}

// Makes sure discovery has found the chat RPC and a live GameState, running a fresh pass when
// the last one is missing or stale (the world was reloaded).
bool EnsureReady(std::string& error) {
    bool names = false, ready = false;
    if (!GameThread::Run([&] { names = UE::ResolveNames(); }, kJobTimeoutMs)) {
        error = "game thread did not respond";
        return false;
    }
    if (!names) {
        error = "Conan server is not ready for chat: engine names not loaded yet";
        return false;
    }
    if (!GameThread::Run([&] { ready = UE::Ready(); }, kJobTimeoutMs)) {
        error = "game thread did not respond";
        return false;
    }
    if (ready) return true;
    GameThread::Run([] { UE::ResetDiscovery(); }, kJobTimeoutMs);
    for (int i = 0; i < kMaxDiscoverySteps; i++) {
        bool done = false;
        if (!GameThread::Run([&] { done = UE::DiscoverStep(); }, kJobTimeoutMs)) {
            error = "game thread did not respond";
            return false;
        }
        if (done) break;
    }
    std::string why = "game thread did not respond";
    GameThread::Run([&] { ready = UE::Ready(); why = UE::DiscoveryError(); }, kJobTimeoutMs);
    if (!ready) error = "Conan server is not ready for chat: " + why;
    return ready;
}

bool Matches(const UE::Controller& c, const std::string& recipient) {
    if (IsSteam64(recipient)) return c.userId == recipient;
    return c.userId == recipient || EqualsIgnoreCase(c.playerName, recipient);
}
}  // namespace

ChatOutcome SendChat(const ChatRequest& cmd) {
    if (cmd.message.empty()) return Fail("sendMessage needs a non-empty message");
    std::string error;
    if (!EnsureReady(error)) return Fail(error);

    // Everything the game thread needs is prepared here, off it.
    const std::u16string user = ChatText(cmd.sender, 64);
    const std::u16string channel = u"Global";
    const std::u16string message = ChatText(cmd.message);
    int64_t sec;
    long nsec;
    WallClock(sec, nsec);
    const uint64_t ticks = FileTimeTicks(sec, nsec);

    int online = 0, delivered = 0;
    std::string seen;  // who was online when a targeted send matched nobody (Steam64 and name only)
    bool stale = false;
    uint64_t jobNs = 0;
    bool ran = GameThread::Run(
        [&] {
            const uint64_t t0 = NowNs();
            // The world may have changed since EnsureReady's job ran.
            if (!UE::Ready()) {
                stale = true;
                return;
            }
            void* func = UE::ChatFunction();
            for (const auto& c : UE::OnlineControllers()) {
                online++;
                if (!cmd.recipient.empty() && !Matches(c, cmd.recipient)) {
                    if (seen.size() < 512) seen += (seen.empty() ? "" : ", ") + c.userId + "/" + c.playerName;
                    continue;
                }
                alignas(16) uint8_t parms[ChatRpc::kSize];
                PackChatRpc(parms, ticks, user, channel, message);
                UE::CallProcessEvent((void*)c.object, func, parms);
                delivered++;
            }
            jobNs = NowNs() - t0;
        },
        kJobTimeoutMs);
    if (!ran) return Fail("game thread did not respond");
    if (stale) return Fail("Conan server is not ready for chat: the world changed, try again");

    NativeLog("sendMessage %s: online=%d delivered=%d game-thread=%.3f ms%s%s",
              cmd.recipient.empty() ? "global" : "targeted", online, delivered, jobNs / 1e6,
              delivered == 0 && !seen.empty() ? "; online userId/name: " : "", delivered == 0 ? seen.c_str() : "");
    ChatOutcome o;
    o.online = online;
    o.delivered = delivered;
    o.gameThreadMs = jobNs / 1e6;
    if (online == 0) o.error = "No online Conan players are available for chat";
    else if (delivered == 0) o.error = "Recipient " + cmd.recipient + " is not online";
    else o.success = true;
    return o;
}

}  // namespace conan
