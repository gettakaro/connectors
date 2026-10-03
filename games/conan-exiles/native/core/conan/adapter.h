// The Conan game half behind takaro::Game: dispatches Takaro actions by the coverage registry,
// owns the event queue the game hooks feed, and holds the refusal state of an unknown build.
#pragma once
#include "conan/chat.h"
#include "takaro/game.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace conan {

struct AdapterOptions {
    bool ready = false;           // build pinned and the ProcessEvent hook installed
    std::string refusal;          // why not ready (unknown build, scan failure, hook failure)
    std::string buildId, build;   // for health and the critical notice
    std::string pinsDetail;       // one line per signature, for health
    double pinsScanMs = 0;
    std::string version;          // library version
    // sendMessage backend; defaults to SendChat (the game thread). Host tests inject a fake.
    std::function<ChatOutcome(const ChatRequest&)> chat;
    // Read actions (lane L2a, conan/reads.h): returns false when it does not handle `action`.
    // Defaults to the production ReadService; host tests inject a fake.
    std::function<bool(const std::string& action, const JsonValue& args, takaro::ActionResult& out)> reads;
    std::function<std::string()> readsHealth;
};

// sendMessage arguments as Takaro sends them: { message, opts: { recipient: { gameId }, senderNameOverride } }
// plus the flat and nested player shapes modules use. Returns false with `error` when message is missing.
bool ParseSendMessage(const JsonValue& args, ChatRequest& out, std::string& error);

class Adapter : public takaro::Game {
public:
    explicit Adapter(AdapterOptions o);
    takaro::ActionResult Execute(const std::string& action, const JsonValue& args) override;
    void DrainEvents(std::vector<takaro::GameEvent>& out) override;
    std::string HealthJson() override;
    // Any thread: queue an event for Takaro (bounded; the oldest `log` event is dropped first).
    void Emit(takaro::GameEvent ev);

    static constexpr size_t kMaxQueuedEvents = 4096;

private:
    takaro::ActionResult Fail(const std::string& error);
    AdapterOptions o_;
    std::mutex mu_;
    std::deque<takaro::GameEvent> events_;
    uint64_t dropped_ = 0, chatSent_ = 0, chatFailed_ = 0;
    double lastChatMs_ = 0;
};

}  // namespace conan
