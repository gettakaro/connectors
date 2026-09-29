// Pure parsing helpers for the VEIN server log (lane L2). See events_parse.h.
#include "events_parse.h"

#include <cctype>
#include <cstring>

namespace {

bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isdigit((unsigned char)c)) return false;
    return true;
}

bool AllHex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isxdigit((unsigned char)c)) return false;
    return true;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

// Removes the value of `?<key>=` / `<key>=` up to the next delimiter, in place.
void MaskAfter(std::string& s, const char* key, const char* delims) {
    size_t klen = strlen(key);
    size_t pos = 0;
    while ((pos = s.find(key, pos)) != std::string::npos) {
        size_t v = pos + klen;
        size_t end = v;
        while (end < s.size() && !strchr(delims, s[end])) end++;
        if (end > v) {
            s.replace(v, end - v, "<redacted>");
            pos = v + 10;
        } else {
            pos = v;
        }
    }
}

}  // namespace

EventsParse::LogLine EventsParse::SplitLogLine(const std::string& raw) {
    LogLine out;
    std::string s = raw;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t body = 0;
    if (!s.empty() && s[0] == '[') {
        size_t t1 = s.find(']');
        if (t1 != std::string::npos && t1 + 1 < s.size() && s[t1 + 1] == '[') {
            size_t t2 = s.find(']', t1 + 2);
            if (t2 != std::string::npos) {
                out.hasPrefix = true;
                out.timestamp = s.substr(1, t1 - 1);
                out.frame = Trim(s.substr(t1 + 2, t2 - (t1 + 2)));
                body = t2 + 1;
            }
        }
    }
    std::string rest = s.substr(body);
    // "Category: message" - the category is a bare identifier, so a colon inside the message never
    // wins (a chat message is full of colons).
    size_t colon = rest.find(": ");
    if (colon != std::string::npos && colon > 0 && colon <= 48) {
        bool ident = true;
        for (size_t i = 0; i < colon; i++)
            if (!isalnum((unsigned char)rest[i]) && rest[i] != '_') { ident = false; break; }
        if (ident) {
            out.category = rest.substr(0, colon);
            out.message = rest.substr(colon + 2);
            return out;
        }
    }
    out.message = rest;
    return out;
}

bool EventsParse::LooksLikeSteamId64(const std::string& s) {
    return s.size() == 17 && AllDigits(s) && s.compare(0, 7, "7656119") == 0;
}

EventsParse::ChatLine EventsParse::ParseChatLine(const std::string& raw) {
    ChatLine out;
    LogLine l = SplitLogLine(raw);
    if (l.category != "LogVeinChat") return out;
    const std::string& m = l.message;
    if (m.empty() || m[0] != '[') return out;
    size_t idEnd = m.find(']');
    if (idEnd == std::string::npos) return out;
    std::string id = m.substr(1, idEnd - 1);
    if (!LooksLikeSteamId64(id)) return out;
    // "<persona>[ (aka <character>)]: <message>"
    size_t nameStart = idEnd + 1;
    while (nameStart < m.size() && m[nameStart] == ' ') nameStart++;
    size_t sep = m.find(": ", nameStart);
    if (sep == std::string::npos) {
        // an empty message still ends with ':'
        sep = m.rfind(':');
        if (sep == std::string::npos || sep < nameStart) return out;
        out.msg = "";
    } else {
        out.msg = m.substr(sep + 2);
    }
    std::string who = Trim(m.substr(nameStart, sep - nameStart));
    size_t aka = who.rfind(" (aka ");
    if (aka != std::string::npos && !who.empty() && who.back() == ')') {
        out.platformName = Trim(who.substr(0, aka));
        out.characterName = Trim(who.substr(aka + 6, who.size() - (aka + 6) - 1));
    } else {
        out.platformName = who;
    }
    if (out.platformName.empty()) return out;
    out.gameId = id;
    out.ok = true;
    return out;
}

EventsParse::CharacterSelectLine EventsParse::ParseCharacterSelectLine(const std::string& raw) {
    CharacterSelectLine out;
    LogLine l = SplitLogLine(raw);
    if (l.category != "LogVein") return out;
    static const char* kMark = "Player ";
    static const char* kSel = " selected character ";
    size_t p = l.message.find(kMark);
    size_t sel = l.message.find(kSel);
    if (p == std::string::npos || sel == std::string::npos || sel < p) return out;
    out.platformName = Trim(l.message.substr(p + strlen(kMark), sel - (p + strlen(kMark))));
    size_t idStart = sel + strlen(kSel);
    size_t idEnd = idStart;
    while (idEnd < l.message.size() && isxdigit((unsigned char)l.message[idEnd])) idEnd++;
    out.characterId = l.message.substr(idStart, idEnd - idStart);
    if (!AllHex(out.characterId) || out.characterId.size() != 32) return out;
    size_t aka = l.message.find("(aka ", idEnd);
    if (aka != std::string::npos) {
        size_t end = l.message.find(')', aka);
        if (end != std::string::npos) out.characterName = Trim(l.message.substr(aka + 5, end - (aka + 5)));
    }
    if (out.platformName.empty()) return out;
    out.ok = true;
    return out;
}

namespace {
// Value of `?<key>=` up to the next '?' or whitespace. VEIN's login URL uses '?' as its separator
// and doubles it before ID (`??ID=`), so an empty segment must be skipped, not treated as a value.
std::string UrlParam(const std::string& s, const char* key) {
    size_t klen = strlen(key);
    size_t pos = 0;
    while ((pos = s.find(key, pos)) != std::string::npos) {
        size_t v = pos + klen;
        size_t end = v;
        while (end < s.size() && s[end] != '?' && s[end] != ' ' && s[end] != '\t') end++;
        if (end > v) return s.substr(v, end - v);
        pos = v;
    }
    return "";
}
}  // namespace

EventsParse::JoinLine EventsParse::ParseJoinLine(const std::string& raw) {
    JoinLine out;
    LogLine l = SplitLogLine(raw);
    if (l.category == "LogNet" &&
        (l.message.compare(0, 15, "Login request: ") == 0 || l.message.compare(0, 14, "Join request: ") == 0)) {
        std::string id = UrlParam(l.message, "?ID=");
        if (!LooksLikeSteamId64(id)) return out;
        out.gameId = id;
        out.name = UrlParam(l.message, "?Name=");
        out.ok = true;
        return out;
    }
    if (l.category == "LogVein") {
        static const char* kMark = "Player ";
        static const char* kTail = " authenticated successfully";
        size_t p = l.message.find(kMark);
        if (p != 0 || l.message.find(kTail) == std::string::npos) return out;
        size_t idStart = strlen(kMark);
        size_t idEnd = idStart;
        while (idEnd < l.message.size() && isdigit((unsigned char)l.message[idEnd])) idEnd++;
        std::string id = l.message.substr(idStart, idEnd - idStart);
        if (!LooksLikeSteamId64(id)) return out;
        out.gameId = id;
        out.ok = true;
        return out;
    }
    return out;
}

EventsParse::LoginRefusedLine EventsParse::ParseLoginRefusedLine(const std::string& raw) {
    LoginRefusedLine out;
    LogLine l = SplitLogLine(raw);
    if (l.category != "LogVein") return out;
    static const char* kMark = "Player ";
    static const char* kTail = " failed to authenticate";
    if (l.message.compare(0, strlen(kMark), kMark) != 0) return out;
    size_t tail = l.message.find(kTail);
    if (tail == std::string::npos) return out;
    size_t idStart = strlen(kMark);
    size_t idEnd = idStart;
    while (idEnd < l.message.size() && isdigit((unsigned char)l.message[idEnd])) idEnd++;
    std::string id = l.message.substr(idStart, idEnd - idStart);
    if (!LooksLikeSteamId64(id)) return out;
    out.gameId = id;
    size_t colon = l.message.find(':', tail);
    if (colon != std::string::npos) {
        size_t r = colon + 1;
        while (r < l.message.size() && l.message[r] == ' ') r++;
        out.reason = l.message.substr(r);
    }
    out.ok = true;
    return out;
}

EventsParse::PlayerStateIdLine EventsParse::ParsePlayerStateIdLine(const std::string& raw) {
    PlayerStateIdLine out;
    LogLine l = SplitLogLine(raw);
    if (l.category != "LogVein") return out;
    static const char* kMark = "PlayerState ID changed to ";
    size_t p = l.message.find(kMark);
    if (p == std::string::npos) return out;
    std::string id = Trim(l.message.substr(p + strlen(kMark)));
    if (!LooksLikeSteamId64(id)) return out;  // the game emits an empty variant first
    out.gameId = id;
    out.ok = true;
    return out;
}

std::string EventsParse::RedactLogLine(const std::string& in) {
    // Two shapes, because one redactor cannot serve both.
    //
    // 1. A URL line - `LogNet: Login request: ?Password=<pw>?Name=<n>??ID=<id>?Ticket=<t> userId: ...`.
    //    Its fields are separated by '?', which common.cpp's Redact() does not treat as a value
    //    terminator, so Redact() alone eats the name and the SteamID64 along with the password.
    //    Here the '?'-delimited secrets are masked individually and Redact() is deliberately NOT run
    //    on the line, so the id and the display name - which are not secrets and which the join
    //    fallback needs - survive. Every key that could carry a secret is masked, not just the two
    //    seen so far.
    // 2. Everything else - the ini / JSON / CLI forms Redact() already handles, plus the URL forms
    //    in case one turns up outside a login line.
    const bool urlLine = (in.find("Login request:") != std::string::npos ||
                          in.find("Join request:") != std::string::npos) &&
                         in.find("?ID=") != std::string::npos;
    std::string s = in;
    static const char* kUrlKeys[] = {"Password=", "password=", "Ticket=", "ticket=", "AuthTicket=",
                                     "Token=",    "token=",    "?p=",     "Secret=", "secret="};
    for (const char* k : kUrlKeys) MaskAfter(s, k, "?& \t");
    if (!urlLine) s = Redact(s);
    return s;
}

bool EventsParse::IsNoise(const std::string& line) {
    if (line.empty()) return true;
    static const char* kNoise[] = {
        "LogSteamShared: Verbose", "LogOnlineVoice: Verbose", "LogNetTraffic", "LogNetSerialization",
        "LogPackageName: Verbose", "LogStreaming: Display: Flushing", "LogVeinMusic",
        "[S_API FAIL]", "LogSlate: Verbose",
    };
    for (const char* n : kNoise)
        if (line.find(n) != std::string::npos) return true;
    return false;
}

// ---- lane L3e: chat channel ------------------------------------------------------------------

const char* EventsParse::ChatSegmentName(int v) {
    switch (v) {
        case 0: return "All";
        case 1: return "Local";
        case 2: return "Global";
        case 3: return "Radio";
        default: return nullptr;
    }
}

const char* EventsParse::ChatChannelName(int v) {
    switch (v) {
        case 0: return "all";
        case 1: return "local";
        case 2: return "global";
        case 3: return "radio";
        default: return "global";
    }
}
