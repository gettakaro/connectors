// Pure part of the #203 research probe (no engine calls): config, matching, stub, in-flight table.
#include "command_probe.h"

namespace CommandProbe {

Classified Classify(const Config& cfg, const std::string& msg, const std::string& senderPuid) {
    Classified c;
    if (!cfg.enabled || msg.size() < 2) return c;
    if (msg[0] != '@' && msg[0] != '/') return c;
    const std::string stem = "takaro_probe_";
    if (msg.compare(1, stem.size(), stem) != 0) return c;  // not probe-shaped: plain chat, no note
    std::string rest = msg.substr(1 + stem.size());
    std::string nonce = rest, mode;
    size_t sp = rest.find(' ');
    if (sp != std::string::npos) {
        nonce = rest.substr(0, sp);
        mode = rest.substr(sp + 1);
    }
    if (nonce != cfg.nonce) { c.reason = "nonce mismatch"; return c; }
    if (!mode.empty() && mode != "ok" && mode != "deny" && mode != "error" && mode != "timeout") {
        c.reason = "unknown mode";
        return c;
    }
    std::string who = senderPuid;
    for (auto& ch : who) ch = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
    if (who != cfg.player) { c.reason = "sender is not the probe player"; return c; }
    c.match = Match::Probe;
    c.prefix = msg[0];
    c.mode = mode.empty() ? "ok" : mode;
    return c;
}

const char* OutcomeName(Outcome o) {
    switch (o) {
        case Outcome::Success: return "success";
        case Outcome::Denied: return "denied";
        case Outcome::Error: return "error";
        case Outcome::Unavailable: return "unavailable";
        case Outcome::Timeout: return "timeout";
    }
    return "?";
}

StubAnswer Stub(const Config& cfg, const std::string& mode) {
    StubAnswer a;
    if (!cfg.stubUp) { a.outcome = Outcome::Unavailable; return a; }
    if (mode == "deny") a.outcome = Outcome::Denied;
    else if (mode == "error") a.outcome = Outcome::Error;
    else if (mode == "timeout") { a.outcome = Outcome::Success; a.delayMs = cfg.deadlineMs + 3000; }
    else a.delayMs = 150;  // a realistic round trip, still well inside the deadline
    return a;
}

uint64_t Pending::Begin(uint64_t nowMs, uint32_t deadlineMs) {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& s : slots_) {
        if (s.id && s.deadline + 10000 < nowMs) s.id = 0;  // a lost timer never pins a slot forever
        if (s.id) continue;
        s.id = next_++;
        s.deadline = nowMs + deadlineMs;
        return s.id;
    }
    return 0;
}

bool Pending::Settle(uint64_t id) {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& s : slots_) {
        if (s.id != id || !id) continue;
        s.id = 0;
        return true;
    }
    return false;
}

size_t Pending::InFlight() {
    std::lock_guard<std::mutex> g(mu_);
    size_t n = 0;
    for (auto& s : slots_) n += s.id ? 1 : 0;
    return n;
}

std::string ReplyText(Outcome o, uint64_t id) {
    std::string tag = "[takaro-probe #" + std::to_string(id) + "] ";
    switch (o) {
        case Outcome::Success: return tag + "ok: probe command ran (stub)";
        case Outcome::Denied: return tag + "denied: you may not run this command (stub)";
        case Outcome::Error: return tag + "failed: the command reported an error (stub)";
        case Outcome::Unavailable: return tag + "unavailable: commands are offline, nothing was run";
        case Outcome::Timeout: return tag + "timed out: no answer in time, it will not run later";
    }
    return tag;
}

}  // namespace CommandProbe
