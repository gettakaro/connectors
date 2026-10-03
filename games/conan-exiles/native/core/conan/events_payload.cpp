#include "conan/events_payload.h"

#include "takaro/json_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace conan {
namespace events {

using takaro::JNull;
using takaro::JNum;
using takaro::JObj;
using takaro::JStr;
using takaro::Put;

namespace {
std::string Lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
bool Contains(const std::string& hay, const char* needle) { return Lower(hay).find(needle) != std::string::npos; }
std::string TrimWs(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

const char* const kPlayerKeys[] = {"gameId", "name", "steamId", "epicOnlineServicesId", "xboxLiveId",
                                   "platformId", "ip", "ping"};
const char* const kPositionKeys[] = {"x", "y", "z"};

struct EventKeys {
    const char* type;
    std::vector<const char*> keys;
};
// Takaro BaseEvent (timestamp, msg) plus each event's own fields.
const EventKeys kEvents[] = {
    {"player-connected", {"player", "msg", "timestamp"}},
    {"player-disconnected", {"player", "msg", "timestamp"}},
    {"chat-message", {"player", "channel", "recipient", "msg", "timestamp"}},
    {"player-death", {"player", "attacker", "position", "msg", "timestamp"}},
    {"entity-killed", {"player", "entity", "weapon", "msg", "timestamp"}},
    {"log", {"msg", "timestamp"}},
};

template <size_t N>
JsonValue Filter(const JsonValue& in, const char* const (&keys)[N]) {
    JsonValue out = JObj();
    if (in.type != JsonValue::Object) return JNull();
    for (auto& kv : in.obj)
        for (auto* k : keys)
            if (kv.first == k) out.obj.push_back(kv);
    return out;
}
}  // namespace

bool PlayerId::Valid() const {
    if (steam64.size() != 17 || steam64.compare(0, 4, "7656") != 0) return false;
    return std::all_of(steam64.begin(), steam64.end(), [](char c) { return c >= '0' && c <= '9'; });
}

JsonValue PlayerJson(const PlayerId& p) {
    JsonValue o = JObj();
    Put(o, "gameId", JStr(p.steam64));
    std::string name = TrimWs(p.name);
    Put(o, "name", JStr(name.empty() ? p.steam64 : name));
    Put(o, "steamId", JStr(p.steam64));
    Put(o, "platformId", JStr("steam:" + p.steam64));
    if (!TrimWs(p.ip).empty()) Put(o, "ip", JStr(TrimWs(p.ip)));
    return o;
}

std::string MapChannel(const std::string& ch) {
    const std::string c = Lower(TrimWs(ch));
    if (c == "clan" || c == "team" || c == "guild") return "team";
    return "global";
}

JsonValue ConnectedPayload(const PlayerId& p) {
    if (!p.Valid()) return JNull();
    JsonValue o = JObj();
    Put(o, "player", PlayerJson(p));
    return o;
}

JsonValue ChatPayload(const PlayerId& p, const std::string& conanChannel, const std::string& msg) {
    if (!p.Valid() || TrimWs(msg).empty()) return JNull();
    JsonValue o = JObj();
    Put(o, "player", PlayerJson(p));
    Put(o, "channel", JStr(MapChannel(conanChannel)));
    Put(o, "msg", JStr(msg));
    return o;
}

JsonValue DeathPayload(const PlayerId& victim, const PlayerId* attacker, const Position& pos, const std::string& msg) {
    if (!victim.Valid()) return JNull();
    JsonValue o = JObj();
    Put(o, "player", PlayerJson(victim));
    if (attacker && attacker->Valid() && attacker->steam64 != victim.steam64) Put(o, "attacker", PlayerJson(*attacker));
    if (pos.has && std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z)) {
        JsonValue p = JObj();
        Put(p, "x", JNum(std::round(pos.x * 100) / 100));
        Put(p, "y", JNum(std::round(pos.y * 100) / 100));
        Put(p, "z", JNum(std::round(pos.z * 100) / 100));
        Put(o, "position", p);
    }
    if (!TrimWs(msg).empty()) Put(o, "msg", JStr(msg));
    return o;
}

JsonValue KilledPayload(const PlayerId& killer, const std::string& entity, const std::string& weapon) {
    if (!killer.Valid() || TrimWs(entity).empty()) return JNull();
    JsonValue o = JObj();
    Put(o, "player", PlayerJson(killer));
    Put(o, "entity", JStr(TrimWs(entity)));
    std::string w = TrimWs(weapon);
    Put(o, "weapon", JStr(w.empty() ? "unknown" : w));  // required string in Takaro's DTO
    return o;
}

JsonValue LogPayload(const std::string& line) {
    if (TrimWs(line).empty()) return JNull();
    JsonValue o = JObj();
    Put(o, "msg", JStr(line));
    return o;
}

JsonValue Sanitize(const std::string& type, const JsonValue& data) {
    if (data.type != JsonValue::Object) return JNull();
    for (auto& e : kEvents) {
        if (type != e.type) continue;
        JsonValue out = JObj();
        for (auto& kv : data.obj) {
            if (std::find_if(e.keys.begin(), e.keys.end(), [&](const char* k) { return kv.first == k; }) ==
                e.keys.end())
                continue;
            if (kv.first == "player" || kv.first == "attacker" || kv.first == "recipient") {
                JsonValue p = Filter(kv.second, kPlayerKeys);
                if (p.type == JsonValue::Object) out.obj.push_back({kv.first, p});
            } else if (kv.first == "position") {
                JsonValue p = Filter(kv.second, kPositionKeys);
                if (p.type == JsonValue::Object && p.obj.size() == 3) out.obj.push_back({kv.first, p});
            } else if (kv.second.type == JsonValue::String) {
                out.obj.push_back(kv);
            }
        }
        return out;
    }
    return JNull();
}

std::string CauseFromDamageType(const std::string& cls) {
    struct Rule {
        const char* needle;
        const char* cause;
    };
    static const Rule kRules[] = {
        {"fall", "fall damage"},   {"drown", "drowning"},     {"starv", "starvation"}, {"hunger", "starvation"},
        {"thirst", "dehydration"}, {"dehydr", "dehydration"}, {"poison", "poison"},    {"bleed", "bleeding"},
        {"fire", "fire"},          {"burn", "fire"},          {"corrupt", "corruption"}, {"sandstorm", "sandstorm"},
        {"cold", "cold"},          {"heat", "heat"},          {"temperature", "temperature"}, {"gas", "gas"},
    };
    for (auto& r : kRules)
        if (Contains(cls, r.needle)) return r.cause;
    return "";
}

std::string DeathMessage(const std::string& victim, const std::string& killer, const std::string& cause) {
    std::string v = TrimWs(victim).empty() ? "A player" : TrimWs(victim);
    if (!TrimWs(killer).empty()) return v + " was killed by " + TrimWs(killer);
    if (!cause.empty()) return v + " died (" + cause + ")";
    return v + " died";
}

bool IsInternalName(const std::string& name) {
    std::string n = Lower(TrimWs(name));
    if (n.empty()) return true;
    size_t x = 0;
    while (x < n.size() && n[x] == 'x') x++;
    if (x >= 2 && x < n.size() && (n[x] == '_' || n[x] == ' ')) return true;
    for (auto* p : {"dev_", "dev ", "test_", "test ", "deprecated", "do not use"})
        if (n.compare(0, strlen(p), p) == 0) return true;
    return false;
}

std::string WeaponName(const std::string& itemName, int32_t templateId) {
    const std::string n = TrimWs(itemName);
    if (templateId == 51204 || templateId == 51205 || Contains(n, "unarmed")) return "Unarmed";
    if (IsInternalName(n)) return "";
    return n;
}

// ------------------------------------------------------------------------------------- log tail

std::string CleanUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; k++) ok = ((unsigned char)s[i + k] >> 6) == 2;
        if (ok && n == 2 && c < 0xC2) ok = false;  // overlong
        if (!ok) {
            out += '?';
            i++;
            continue;
        }
        if (n == 1 && c < 0x20 && c != '\t') out += ' ';
        else out.append(s, i, n);
        i += n;
    }
    return out;
}

std::vector<std::string> LineSplitter::Feed(const std::string& bytes) {
    std::vector<std::string> lines;
    partial_ += bytes;
    size_t start = 0;
    for (;;) {
        size_t nl = partial_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = partial_.substr(start, nl - start);
        start = nl + 1;
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) continue;
        if (line.size() > max_) {
            size_t cut = max_;
            while (cut > 0 && ((unsigned char)line[cut] >> 6) == 2) cut--;  // UTF-8 boundary
            line = line.substr(0, cut) + "...";
        }
        lines.push_back(CleanUtf8(line));
    }
    partial_.erase(0, start);
    // A runaway line without a newline: flush what we have instead of growing without bound.
    if (partial_.size() > 64 * 1024) {
        lines.push_back(CleanUtf8(partial_.substr(0, max_)) + "...");
        partial_.clear();
    }
    return lines;
}

std::string RedactLine(const std::string& line, const std::vector<std::string>& secrets) {
    std::string s = line;
    for (auto& sec : secrets) {
        if (sec.size() < 6) continue;
        for (size_t at = s.find(sec); at != std::string::npos; at = s.find(sec, at + 10)) s.replace(at, sec.size(), "[redacted]");
    }
    // key=value / key: value with a secret-looking key.
    const std::string low = Lower(s);
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t best = std::string::npos;
        size_t keyEnd = 0;
        for (auto* k : {"password", "passwd", "token", "secret"}) {
            size_t at = low.find(k, i);
            if (at == std::string::npos) continue;
            // extend over the rest of an identifier (RconPassword, identityToken, ...)
            size_t e = at + strlen(k);
            while (e < s.size() && (isalnum((unsigned char)s[e]) || s[e] == '_')) e++;
            if (at < best) {
                best = at;
                keyEnd = e;
            }
        }
        if (best == std::string::npos) break;
        size_t v = keyEnd;
        while (v < s.size() && (s[v] == ' ' || s[v] == '"' || s[v] == '\'')) v++;
        if (v < s.size() && (s[v] == '=' || s[v] == ':')) {
            v++;
            while (v < s.size() && (s[v] == ' ' || s[v] == '"' || s[v] == '\'')) v++;
            size_t e = v;
            while (e < s.size() && s[e] != ' ' && s[e] != '?' && s[e] != '&' && s[e] != '"' && s[e] != '\'' &&
                   s[e] != ',' && s[e] != ';')
                e++;
            out.append(s, i, v - i);
            if (e > v) out += "[redacted]";
            i = e;
        } else {
            out.append(s, i, keyEnd - i);
            i = keyEnd;
        }
    }
    out.append(s, i, std::string::npos);
    return out;
}

bool RateLimiter::Allow(uint64_t now) {
    if (last_ == 0) last_ = now;
    if (now > last_) {
        tokens_ = std::min(burst_, tokens_ + (double)(now - last_) * rate_ / 1000.0);
        last_ = now;
    }
    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        return true;
    }
    dropped_++;
    return false;
}

uint64_t RateLimiter::TakeDroppedSinceLast() {
    uint64_t d = dropped_ - reported_;
    reported_ = dropped_;
    return d;
}

}  // namespace events
}  // namespace conan
