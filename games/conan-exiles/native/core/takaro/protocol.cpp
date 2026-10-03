#include "takaro/protocol.h"

#include "takaro/json_util.h"

#include <cstdio>
#include <cstdlib>

namespace takaro {

const char* const kActions[kActionCount] = {
    "getPlayer",     "getPlayers",   "getPlayerLocation", "getPlayerInventory", "giveItem",
    "listItems",     "listEntities", "listLocations",     "executeConsoleCommand", "sendMessage",
    "teleportPlayer", "testReachability", "kickPlayer",   "banPlayer",          "unbanPlayer",
    "listBans",      "shutdown",     "getMapInfo",        "getMapTile"};
const char* const kEventTypes[kEventTypeCount] = {"log",          "player-connected", "player-disconnected",
                                                  "chat-message", "player-death",     "entity-killed"};

bool IsAction(const std::string& action) {
    for (auto* a : kActions)
        if (action == a) return true;
    return false;
}

bool IsEventType(const std::string& type) {
    for (auto* t : kEventTypes)
        if (type == t) return true;
    return false;
}

JsonValue NormalizeArgs(const JsonValue* value) {
    if (!value || value->type == JsonValue::Null || value->type == JsonValue::Array) return JObj();
    if (value->type == JsonValue::String) {
        std::string t = Trim(value->str);
        if (t.empty()) return JObj();
        JsonValue parsed;
        if (!ParseJson(t, parsed)) return JObj();
        return AsRecord(&parsed);
    }
    return AsRecord(value);
}

std::string CreateIdentify(const std::string& identityToken, const std::string& registrationToken,
                           const std::string& serverName) {
    ObjBuilder p;
    p.S("identityToken", identityToken);
    if (!registrationToken.empty()) p.S("registrationToken", registrationToken);
    if (!serverName.empty()) p.S("name", serverName);
    return ObjBuilder().S("type", "identify").Raw("payload", p.Done()).Done();
}

std::string CreateResponse(const std::string& requestId, const JsonValue& payload) {
    return ObjBuilder()
        .S("type", "response")
        .S("requestId", requestId)
        .Raw("payload", payload.type == JsonValue::Null ? "{}" : JsonDump(payload))
        .Done();
}

std::string CreateErrorResponse(const std::string& requestId, const std::string& error) {
    return ObjBuilder().S("type", "response").S("requestId", requestId).S("error", error).Done();
}

std::string CreateGameEvent(const std::string& type, const JsonValue& data) {
    return ObjBuilder().S("type", "gameEvent").Raw("payload", ObjBuilder().S("type", type).Raw("data", JsonDump(data)).Done()).Done();
}

std::string PlayerId(const JsonValue& args) {
    const JsonValue* sources[] = {&args, &AsRecord(args.get("player")), &AsRecord(args.get("playerRef"))};
    for (const JsonValue* s : sources)
        for (auto* key : {"gameId", "steamId", "platformId"})
            if (auto id = Str(s->get(key))) return *id;
    return "";
}

std::string StripSteamPrefix(const std::string& id) {
    std::string t = Trim(id);
    size_t colon = t.rfind(':');
    if (colon == std::string::npos) return t;
    std::string head = Lower(t.substr(0, colon + 1));
    if (head == "steam:" || head == "platform:steam:") return t.substr(colon + 1);
    return t;
}

namespace {
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
}  // namespace

bool ParseIsoMs(const std::string& raw, int64_t& ms) {
    std::string t = Trim(raw);
    if (t.empty()) return false;
    bool digits = true;
    for (char c : t) digits = digits && c >= '0' && c <= '9';
    if (digits) {
        if (t.size() > 15) return false;
        ms = strtoll(t.c_str(), nullptr, 10);
        return true;
    }
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, s = 0, frac = 0, fracDigits = 0;
    size_t i = 0;
    auto num = [&](int width, int& out) {
        if (i + width > t.size()) return false;
        out = 0;
        for (int k = 0; k < width; k++) {
            char c = t[i + k];
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
        }
        i += width;
        return true;
    };
    if (!num(4, Y) || i >= t.size() || t[i++] != '-' || !num(2, M) || i >= t.size() || t[i++] != '-' || !num(2, D))
        return false;
    int64_t offsetMin = 0;
    if (i < t.size()) {
        if (t[i] != 'T' && t[i] != 't' && t[i] != ' ') return false;
        i++;
        if (!num(2, h) || i >= t.size() || t[i++] != ':' || !num(2, mi)) return false;
        if (i < t.size() && t[i] == ':') {
            i++;
            if (!num(2, s)) return false;
            if (i < t.size() && t[i] == '.') {
                i++;
                while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
                    if (fracDigits < 3) frac = frac * 10 + (t[i] - '0');
                    fracDigits++;
                    i++;
                }
                if (!fracDigits) return false;
                for (int k = fracDigits; k < 3; k++) frac *= 10;
            }
        }
        if (i < t.size() && (t[i] == 'Z' || t[i] == 'z')) {
            i++;
        } else if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
            int sign = t[i] == '-' ? -1 : 1, oh = 0, om = 0;
            i++;
            if (!num(2, oh)) return false;
            if (i < t.size() && t[i] == ':') i++;
            if (!num(2, om)) return false;
            offsetMin = sign * (oh * 60 + om);
        } else {
            return false;  // a local time without zone is ambiguous on a server: refuse it
        }
        if (i != t.size()) return false;
    }
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || s > 60) return false;
    int64_t days = DaysFromCivil(Y, (unsigned)M, (unsigned)D);
    ms = ((days * 24 + h) * 60 + mi) * 60000LL + s * 1000LL + frac - offsetMin * 60000LL;
    return true;
}

std::string FormatIsoMs(int64_t ms) {
    int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
    int64_t rem = ms - days * 86400000;
    // civil_from_days
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    char b[40];
    snprintf(b, sizeof b, "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ", (long long)y, m, d, (int)(rem / 3600000),
             (int)(rem / 60000 % 60), (int)(rem / 1000 % 60), (int)(rem % 1000));
    return b;
}

}  // namespace takaro
