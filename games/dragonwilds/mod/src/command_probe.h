// Research probe for gettakaro/connectors#203 (native/silent Takaro commands). OFF by default.
//
// It recognises exactly one trigger, `@takaro_probe_<nonce>` or `/takaro_probe_<nonce>`, from exactly
// one player (EOS ProductUserId), both taken from config. Only that input is suppressed and answered
// privately by a local stub; every other line, including other probe-shaped lines, stays ordinary
// game chat. This is not a command catalog, a permission system or an invocation API: nothing here
// reaches Takaro. Production code must not depend on it.
//
// Keys (env or takaro.cfg): TAKARO_COMMAND_PROBE=1, TAKARO_COMMAND_PROBE_NONCE (8-32 [a-z0-9]),
// TAKARO_COMMAND_PROBE_PLAYER (32 hex), TAKARO_COMMAND_PROBE_STUB=up|down,
// TAKARO_COMMAND_PROBE_DEADLINE_MS (default 2000, 200-10000).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

namespace CommandProbe {

struct Config {
    bool enabled = false;
    std::string nonce;
    std::string player;  // lowercase 32-hex PUID
    bool stubUp = true;
    uint32_t deadlineMs = 2000;
    std::string error;   // why a requested probe stayed off
};

// Pure: `get` returns the raw value of a key ("" when unset).
template <typename Get>
Config ParseConfig(Get get);

enum class Match { None, Probe };
struct Classified {
    Match match = Match::None;
    char prefix = 0;        // '@' or '/'
    std::string mode;       // ok | deny | error | timeout (empty = ok)
    std::string reason;     // why a probe-shaped line was NOT taken (for the receipt log)
};

// Pure. A line is taken only when it is exactly `<@|/>takaro_probe_<nonce>[ <mode>]`, the mode is
// known, and the sender is the configured player. Anything else returns None (ordinary chat).
Classified Classify(const Config& cfg, const std::string& msg, const std::string& senderPuid);

enum class Outcome { Success, Denied, Error, Unavailable, Timeout };
const char* OutcomeName(Outcome o);

// The stub's answer for a mode: the outcome it will report and after how long. `timeout` answers
// late on purpose (deadline + 3 s) so the late answer can be shown to be dropped.
struct StubAnswer {
    Outcome outcome = Outcome::Success;
    uint32_t delayMs = 0;
};
StubAnswer Stub(const Config& cfg, const std::string& mode);

// One bounded in-flight table. Each taken input gets an id; the first of {stub answer, deadline}
// settles it, and anything after that is dropped and never replayed.
class Pending {
public:
    uint64_t Begin(uint64_t nowMs, uint32_t deadlineMs);
    // true when this call settled the id (the caller then answers the player privately).
    bool Settle(uint64_t id);
    size_t InFlight();

private:
    std::mutex mu_;
    uint64_t next_ = 1;
    static const size_t kMax = 8;
    struct Slot { uint64_t id = 0; uint64_t deadline = 0; };
    Slot slots_[kMax];
};

// Text the player sees privately for a settled outcome.
std::string ReplyText(Outcome o, uint64_t id);

// ---- in-process glue (events.cpp) ----
void Init();                 // reads the config once; logs the state
bool Enabled();
// Called from the chat hook BEFORE the original Server_SendChatMessage runs. Returns true when the
// original must be skipped. `senderPuid` is resolved from the chat component's owner, never from
// the message's SenderData.
bool OnServerChat(const std::string& msg, const std::string& senderPuid);

}  // namespace CommandProbe

// ---- template implementation ----
namespace CommandProbe {
template <typename Get>
Config ParseConfig(Get get) {
    Config c;
    std::string on = get("TAKARO_COMMAND_PROBE");
    if (on != "1" && on != "true") return c;
    c.nonce = get("TAKARO_COMMAND_PROBE_NONCE");
    c.player = get("TAKARO_COMMAND_PROBE_PLAYER");
    for (auto& ch : c.player) ch = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
    std::string stub = get("TAKARO_COMMAND_PROBE_STUB");
    c.stubUp = stub != "down";
    std::string dl = get("TAKARO_COMMAND_PROBE_DEADLINE_MS");
    if (!dl.empty()) {
        char* end = nullptr;
        unsigned long v = strtoul(dl.c_str(), &end, 10);
        if (end && !*end && v >= 200 && v <= 10000) c.deadlineMs = (uint32_t)v;
    }
    auto allOf = [](const std::string& s, const char* set) { return s.find_first_not_of(set) == std::string::npos; };
    if (c.nonce.size() < 8 || c.nonce.size() > 32 || !allOf(c.nonce, "abcdefghijklmnopqrstuvwxyz0123456789"))
        c.error = "TAKARO_COMMAND_PROBE_NONCE must be 8-32 characters [a-z0-9]";
    else if (c.player.size() != 32 || !allOf(c.player, "0123456789abcdef"))
        c.error = "TAKARO_COMMAND_PROBE_PLAYER must be the player's 32-hex EOS ProductUserId";
    else
        c.enabled = true;
    return c;
}
}  // namespace CommandProbe
