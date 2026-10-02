#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include "native_log.h"
#include "common.h"
#include "state.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <cctype>
#include <cstdlib>
#include <map>

namespace NativeLog {
namespace {
using Json = nlohmann::json;
std::string Env(const char* k) { const char* v = getenv(k); return v ? v : ""; }
std::string Trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

struct Re {
    pcre2_code* code = nullptr;
    std::string label;
    Re() = default;
    Re(const Re&) = delete;
    Re& operator=(const Re&) = delete;
    ~Re() { if (code) pcre2_code_free(code); }
    bool Compile(const char* pattern, bool caseless, std::string& error) {
        int e = 0;
        PCRE2_SIZE offset = 0;
        code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern), PCRE2_ZERO_TERMINATED,
                             (caseless ? PCRE2_CASELESS : 0) | PCRE2_UTF | PCRE2_MATCH_INVALID_UTF, &e, &offset,
                             nullptr);
        if (code) return true;
        PCRE2_UCHAR text[256];
        pcre2_get_error_message(e, text, sizeof text);
        error = label + ": PCRE2 at " + std::to_string(offset) + ": " + reinterpret_cast<const char*>(text);
        return false;
    }
    struct Hit {
        bool matched = false, limitExceeded = false;
        std::vector<std::string> groups;  // [0] = group 1
    };
    Hit Match(const std::string& line) const {
        Hit h;
        if (!code) return h;
        auto* context = pcre2_match_context_create(nullptr);
        pcre2_set_match_limit(context, 200000);
        pcre2_set_depth_limit(context, 1000);
        auto* m = pcre2_match_data_create_from_pattern(code, nullptr);
        int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(line.data()), line.size(), 0, 0, m, context);
        pcre2_match_context_free(context);
        if (rc < 0) {
            h.limitExceeded = rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT;
            pcre2_match_data_free(m);
            return h;
        }
        h.matched = true;
        PCRE2_SIZE* v = pcre2_get_ovector_pointer(m);
        uint32_t count = 0;
        pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &count);
        for (uint32_t i = 1; i <= count; i++) {
            if (i < (uint32_t)rc && v[2 * i] != PCRE2_UNSET) h.groups.push_back(line.substr(v[2 * i], v[2 * i + 1] - v[2 * i]));
            else h.groups.push_back("");
        }
        pcre2_match_data_free(m);
        return h;
    }
    // Global substitution with JavaScript-style `$n` references. Returns false on error.
    bool Replace(const std::string& in, const char* replacement, std::string& out, int& replaced) const {
        replaced = 0;
        if (!code) { out = in; return false; }
        std::vector<PCRE2_UCHAR> buf(in.size() * 2 + 256);
        for (int attempt = 0; attempt < 2; attempt++) {
            PCRE2_SIZE outLen = buf.size();
            int rc = pcre2_substitute(code, reinterpret_cast<PCRE2_SPTR>(in.data()), in.size(), 0,
                                      PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH, nullptr, nullptr,
                                      reinterpret_cast<PCRE2_SPTR>(replacement), PCRE2_ZERO_TERMINATED, buf.data(),
                                      &outLen);
            if (rc >= 0) {
                replaced = rc;
                out.assign(reinterpret_cast<const char*>(buf.data()), outLen);
                return true;
            }
            if (rc != PCRE2_ERROR_NOMEMORY) break;
            buf.resize(outLen + 1);
        }
        out = in;
        return false;
    }
};

// Verbatim grammar from the real server (sidecar/src/dragonwilds/logTail.ts, research/log-grammar.md).
const char* kLoginRequest = R"(LogNet:\s*Login request:.*userId:\s*RedpointEOS:([0-9a-f]{32}))";
const char* kUrlName = R"(\?Name=([^?\s]+))";
const char* kEnteredWorld =
    R"(LogDominionPlayerControllerBase:.*PlayerChar entered world\s*\[Account\[XP:([0-9a-f]{32})\][^\]]*\s*Character Name\[(.*?)\](?:\s*Guid\[DCG:([0-9A-Fa-f]+)\])?)";
const char* kDisconnect =
    R"(LogDominionPlayerController:.*ClientRequestDisconnect.*?Account\[XP:([0-9a-f]{32})\][^\]]*\s*Character Name\[(.*?)\])";
const char* kDisconnectNameOnly =
    R"(LogDominionPlayerController:.*ClientRequestDisconnect(?!.*Account\[XP:).*Character Name\[(.*?)\])";
const char* kChannelCleanup = R"(UChannel::CleanUp:.*UniqueId:\s*RedpointEOS:([0-9a-f]{32}))";
// Redaction (sidecar/src/bridge.ts redactLog, extended to Token/Ticket assignments).
const char* kUrlPassword = R"((\?p=)[^?\s]*)";
const char* kPasswordKeys = R"((WorldPassword|AdminPassword|Password))";
const char* kPasswordAssignment = R"(((?:\w*)Password)(\s*[=:]\s*)("?)([^\s",;]*)\3)";
const char* kSecretAssignment = R"(((?:\w*)(?:Token|Ticket))(\s*[=:]\s*)("?)([^\s",;?&]*)\3)";
// Unreal/EOS chatter worthless to an admin: Takaro rate-limits `log` per server, and the server
// prints thousands of Redpoint EOS verbose lines (the plugin's old ring filter plus the sidecar's).
const char* kNoise =
    R"(:\s*(?:Verbose|VeryVerbose):|LogRedpointEOS\w*:\s*Verbose|LogEOSHTTP|LogEOSAnalytics|LogEOSNetworkAuth|LogRedpointEOSHTTP|LogStreamableManager|LogSpudData|SendBackendEvent|^\s*$)";
// Takaro drops `log` events above ~50 per 30 s per server (event-rate-limited). The connector stays
// under that on its own: at most kLogRateMax lines per kLogRateWindowMs, the rest are counted and
// summarised in one line once the window reopens. DRAGONWILDS_LOG_RATE overrides the count (0 = off).
constexpr int64_t kLogRateWindowMs = 30000;
constexpr int kLogRateDefault = 40;
}  // namespace

struct Parser::Impl {
    Re login, urlName, entered, disconnect, disconnectName, cleanup;
    Re urlPassword, passwordKeys, passwordAssignment, secretAssignment, noise;
    std::map<std::string, std::string> present;   // in-world puid -> character name
    std::map<std::string, std::string> loginIds;  // platform name -> puid (Login request:)
    std::map<std::string, std::string> known;     // character name -> puid, for a name-only leave
    std::string mode = "auto", events = "filtered";
    int rateMax = kLogRateDefault;
    std::deque<int64_t> sent;  // steady-clock ms of the log events admitted in the current window
    uint64_t suppressed = 0;
    std::string lastError;
    bool configured = false;
};

Parser::Parser() : impl_(std::make_unique<Impl>()) {}
Parser::~Parser() = default;

bool Parser::Configure(std::string& key, std::string& detail) {
    auto& s = *impl_;
    struct Spec { Re* re; const char* label; const char* pattern; bool caseless; };
    Spec specs[] = {
        {&s.login, "loginRequest", kLoginRequest, true},
        {&s.urlName, "urlName", kUrlName, true},
        {&s.entered, "enteredWorld", kEnteredWorld, true},
        {&s.disconnect, "disconnect", kDisconnect, true},
        {&s.disconnectName, "disconnectNameOnly", kDisconnectNameOnly, true},
        {&s.cleanup, "channelCleanup", kChannelCleanup, true},
        {&s.urlPassword, "urlPassword", kUrlPassword, true},
        {&s.passwordKeys, "passwordKeys", kPasswordKeys, true},
        {&s.passwordAssignment, "passwordAssignment", kPasswordAssignment, true},
        {&s.secretAssignment, "secretAssignment", kSecretAssignment, true},
        {&s.noise, "noise", kNoise, false},
    };
    for (auto& spec : specs) {
        spec.re->label = spec.label;
        if (!spec.re->Compile(spec.pattern, spec.caseless, detail)) {
            key = "DRAGONWILDS_LOG_GRAMMAR";
            return false;
        }
    }
    s.mode = Lower(Trim(Env("DRAGONWILDS_LOG_TAIL")));
    if (s.mode.empty()) s.mode = "auto";
    if (s.mode != "auto" && s.mode != "always" && s.mode != "never") {
        key = "DRAGONWILDS_LOG_TAIL";
        detail = "must be auto|always|never";
        return false;
    }
    s.events = Lower(Trim(Env("DRAGONWILDS_LOG_EVENTS")));
    if (s.events.empty()) s.events = "filtered";
    if (s.events != "all" && s.events != "filtered" && s.events != "none") {
        key = "DRAGONWILDS_LOG_EVENTS";
        detail = "must be all|filtered|none";
        return false;
    }
    std::string rate = Trim(Env("DRAGONWILDS_LOG_RATE"));
    if (!rate.empty()) {
        char* end = nullptr;
        long v = strtol(rate.c_str(), &end, 10);
        if (!end || *end || v < 0 || v > 100000) {
            key = "DRAGONWILDS_LOG_RATE";
            detail = "must be a number of log lines per 30 s (0 = unlimited)";
            return false;
        }
        s.rateMax = (int)v;
    }
    s.configured = true;
    return true;
}

bool Parser::TailConnections() const {
    if (impl_->mode == "never") return false;
    if (impl_->mode == "always") return true;
    // auto: the log tail takes over join/leave while the plugin's own player capability is not ok.
    return PluginState::Get().Capability("players") != "ok";
}

std::string Parser::LastError() const { return impl_->lastError; }

bool Parser::Noise(const std::string& line) const { return impl_->noise.Match(line).matched; }

std::string Parser::Redact(const std::string& line) const {
    auto& s = *impl_;
    std::string out;
    int n = 0;
    if (!s.urlPassword.Replace(line, "$1[redacted]", out, n)) return "[redacted: line could not be redacted]";
    std::string tmp;
    if (!s.secretAssignment.Replace(out, "$1$2[redacted]", tmp, n)) return "[redacted: line could not be redacted]";
    out.swap(tmp);
    if (!s.passwordKeys.Match(out).matched) return out;
    if (!s.passwordAssignment.Replace(out, "$1$2[redacted]", tmp, n)) return "[redacted: line could not be redacted]";
    // A password mentioned without a `key=value` shape (free text) is dropped rather than leaked.
    return n > 0 ? tmp : "[redacted: line mentions a password]";
}

std::vector<Parsed> Parser::Feed(const std::string& raw) {
    auto& s = *impl_;
    std::vector<Parsed> out;
    if (!s.configured) return out;
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.size() > 64 * 1024) line.resize(64 * 1024);
    auto match = [&](const Re& re) {
        auto h = re.Match(line);
        if (h.limitExceeded) s.lastError = re.label + ": PCRE2 match/depth limit exceeded";
        return h;
    };
    if (s.events == "all" || (s.events == "filtered" && !Noise(line))) {
        bool admit = true;
        if (s.rateMax > 0) {
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count();
            while (!s.sent.empty() && nowMs - s.sent.front() >= kLogRateWindowMs) s.sent.pop_front();
            if ((int)s.sent.size() >= s.rateMax) {
                admit = false;
                ++s.suppressed;
            } else {
                if (s.suppressed && (int)s.sent.size() + 1 < s.rateMax) {
                    out.push_back({"log", Json{{"msg", "[takaro] " + std::to_string(s.suppressed) +
                                                          " server log line(s) not forwarded (connector log rate "
                                                          "limit, " + std::to_string(s.rateMax) + " per 30 s)"}}.dump()});
                    s.sent.push_back(nowMs);
                    s.suppressed = 0;
                }
                s.sent.push_back(nowMs);
            }
        }
        if (admit) out.push_back({"log", Json{{"msg", Redact(line)}}.dump()});
    }

    const bool tail = TailConnections();
    auto player = [](const std::string& id, const std::string& name) {
        Json p = Json::object();
        if (!id.empty()) p["gameId"] = id;
        if (!name.empty()) p["name"] = name;
        return Json{{"player", p}}.dump();
    };
    auto bound = [](std::map<std::string, std::string>& m) { while (m.size() > 500) m.erase(m.begin()); };

    if (auto h = match(s.login); h.matched) {
        auto name = s.urlName.Match(line);
        if (name.matched && !name.groups.empty() && !name.groups[0].empty()) {
            s.loginIds[name.groups[0]] = Lower(h.groups[0]);
            bound(s.loginIds);
        }
        return out;
    }
    if (auto h = match(s.entered); h.matched) {
        std::string id = Lower(h.groups[0]), name = Trim(h.groups[1]);
        if (s.present.count(id)) return out;  // character swap / duplicate line: already connected
        s.present[id] = name;
        bound(s.present);
        if (!name.empty()) { s.known[name] = id; bound(s.known); }
        if (tail) out.push_back({"player-connected", player(id, name)});
        return out;
    }
    if (auto h = match(s.disconnect); h.matched) {
        std::string id = Lower(h.groups[0]), name = Trim(h.groups[1]);
        auto it = s.present.find(id);
        if (name.empty() && it != s.present.end()) name = it->second;
        if (it != s.present.end()) s.present.erase(it);
        if (!name.empty()) s.known.erase(name);
        if (tail) out.push_back({"player-disconnected", player(id, name)});
        return out;
    }
    if (auto h = match(s.disconnectName); h.matched) {
        std::string name = Trim(h.groups[0]), id;
        if (name.empty()) return out;
        for (auto it = s.present.begin(); it != s.present.end(); ++it)
            if (it->second == name) { id = it->first; s.present.erase(it); break; }
        if (id.empty()) {
            auto k = s.known.find(name);
            if (k != s.known.end()) id = k->second;
        }
        s.known.erase(name);
        // Without an id the leave cannot be attributed; never guess one from the name.
        if (tail && !id.empty()) out.push_back({"player-disconnected", player(id, name)});
        return out;
    }
    if (auto h = match(s.cleanup); h.matched) {
        // Hard drop (crash/timeout): an event only when no ClientRequestDisconnect was seen.
        std::string id = Lower(h.groups[0]);
        auto it = s.present.find(id);
        if (it == s.present.end()) return out;
        std::string name = it->second;
        s.present.erase(it);
        if (!name.empty()) s.known.erase(name);
        if (tail) out.push_back({"player-disconnected", player(id, name)});
        return out;
    }
    // `LogNet: Join succeeded: <Name>` is the platform display name and never an event; chat is not
    // logged server-side at all, so it only ever comes from the plugin's ProcessEvent hook.
    return out;
}
}  // namespace NativeLog
