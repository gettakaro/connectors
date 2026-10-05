#include "native/mapping.h"

#include "native/json_util.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>

namespace native {

const char* const kGameServerActions[17] = {
    "getPlayer",    "getPlayers",   "getPlayerLocation", "getPlayerInventory", "giveItem",   "listItems",
    "listEntities", "listLocations", "executeConsoleCommand", "sendMessage", "teleportPlayer", "testReachability",
    "kickPlayer",   "banPlayer",    "unbanPlayer",       "listBans",           "shutdown"};
const char* const kGameEventTypes[6] = {"log", "player-connected", "player-disconnected", "chat-message",
                                        "player-death", "entity-killed"};

bool IsGameEventType(const std::string& type) {
    for (auto* t : kGameEventTypes)
        if (type == t) return true;
    return false;
}

void Fail(const std::string& message) { throw NativeError(ErrorKind::Action, message); }

namespace {

std::optional<std::string> SteamFromGameId(const std::optional<std::string>& gameId) {
    if (!gameId || gameId->size() != 17 || gameId->compare(0, 4, "7656") != 0) return std::nullopt;
    for (char c : *gameId)
        if (c < '0' || c > '9') return std::nullopt;
    return gameId;
}

std::optional<std::string> SteamFromPlatform(const std::optional<std::string>& platformId) {
    if (!platformId || platformId->size() <= 6) return std::nullopt;
    if (Lower(platformId->substr(0, 6)) != "steam:") return std::nullopt;
    return SteamFromGameId(platformId->substr(6));
}

template <typename T>
std::optional<T> Or(std::optional<T> a, std::optional<T> b) {
    return a ? a : b;
}

std::string MapChannel(const JsonValue* raw) {
    std::string c = Lower(raw && raw->type != JsonValue::Null ? JsString(raw) : "");
    if (c == "team" || c == "friends" || c == "whisper") return c;
    return "global";
}

}  // namespace

JsonValue MapPlayer(const JsonValue* raw) {
    const JsonValue& p = AsRecord(raw);
    // Only a real SteamID64 is sent as steamId: Takaro matches players on it verbatim, and the
    // plugin's ban list carries the account hash in steamId when the SteamID is not cached.
    auto steamId = Or(Or(SteamFromGameId(Str(p.get("steamId"))), SteamFromPlatform(Str(p.get("platformId")))),
                      SteamFromGameId(Str(p.get("gameId"))));
    auto gameId = Or(Or(steamId, Str(p.get("gameId"))), Str(p.get("name")));
    if (!gameId) throw NativeError(ErrorKind::Plain, "Plugin player has no identifier: " + JsStringify(raw));
    JsonValue player = JObj();
    Put(player, "gameId", JStr(*gameId));
    Put(player, "name", JStr(Or(Str(p.get("name")), gameId).value()));
    if (steamId) {
        Put(player, "steamId", JStr(*steamId));
        Put(player, "platformId", JStr("steam:" + *steamId));
    }
    if (auto ip = Str(p.get("ip"))) Put(player, "ip", JStr(*ip));
    const JsonValue* ping = p.get("ping");
    if (ping && ping->type == JsonValue::Number && std::isfinite(ping->num)) Put(player, "ping", *ping);
    return player;
}

JsonValue MapPosition(const JsonValue* raw) {
    const JsonValue& p = AsRecord(raw);
    auto x = Num(p.get("x")), y = Num(p.get("y")), z = Num(p.get("z"));
    if (!x || !y || !z) throw NativeError(ErrorKind::Plain, "Plugin returned an invalid position: " + JsStringify(raw));
    JsonValue pos = JObj();
    Put(pos, "x", JNum(*x));
    Put(pos, "y", JNum(*y));
    Put(pos, "z", JNum(*z));
    if (auto d = Str(p.get("dimension"))) Put(pos, "dimension", JStr(*d));
    return pos;
}

JsonValue MapInventoryItem(const JsonValue* raw) {
    const JsonValue& i = AsRecord(raw);
    auto code = Or(Str(i.get("code")), Str(i.get("name")));
    if (!code) throw NativeError(ErrorKind::Plain, "Plugin inventory item has no code: " + JsStringify(raw));
    JsonValue item = JObj();
    Put(item, "code", JStr(*code));
    Put(item, "name", JStr(Or(Str(i.get("name")), code).value()));
    Put(item, "amount", JNum(Num(i.get("amount")).value_or(1)));
    const JsonValue* q = i.get("quality");
    if (q && q->type != JsonValue::Null && !(q->type == JsonValue::String && q->str.empty()))
        Put(item, "quality", JStr(JsString(q)));
    return item;
}

JsonValue MapItemDefinition(const JsonValue* raw) {
    const JsonValue& i = AsRecord(raw);
    auto code = Or(Str(i.get("code")), Str(i.get("name")));
    if (!code) throw NativeError(ErrorKind::Plain, "Plugin item has no code: " + JsStringify(raw));
    JsonValue item = JObj();
    Put(item, "code", JStr(*code));
    Put(item, "name", JStr(Or(Str(i.get("name")), code).value()));
    if (auto d = Str(i.get("description"))) Put(item, "description", JStr(*d));
    return item;
}

std::string MapEntityType(const JsonValue* raw) {
    std::string t = Lower(raw && raw->type != JsonValue::Null ? JsString(raw) : "");
    for (auto* k : {"hostile", "enemy", "monster", "aggressive", "shroud"})
        if (t.find(k) != std::string::npos) return "hostile";
    for (auto* k : {"friendly", "ally", "npc", "villager", "survivor", "companion", "pet"})
        if (t.find(k) != std::string::npos) return "friendly";
    return "neutral";
}

JsonValue MapEntity(const JsonValue* raw) {
    const JsonValue& e = AsRecord(raw);
    auto code = Or(Str(e.get("code")), Str(e.get("name")));
    if (!code) throw NativeError(ErrorKind::Plain, "Plugin entity has no code: " + JsStringify(raw));
    JsonValue entity = JObj();
    Put(entity, "code", JStr(*code));
    Put(entity, "name", JStr(Or(Str(e.get("name")), code).value()));
    Put(entity, "type", JStr(MapEntityType(e.get("type"))));
    if (auto d = Str(e.get("description"))) Put(entity, "description", JStr(*d));
    return entity;
}

JsonValue MapLocation(const JsonValue* raw) {
    const JsonValue& l = AsRecord(raw);
    auto code = Or(Str(l.get("code")), Str(l.get("name")));
    if (!code) throw NativeError(ErrorKind::Plain, "Plugin location has no code: " + JsStringify(raw));
    const JsonValue* posSrc = l.get("position");
    if (!posSrc || posSrc->type == JsonValue::Null) posSrc = &l;
    JsonValue loc = JObj();
    Put(loc, "code", JStr(*code));
    Put(loc, "name", JStr(Or(Str(l.get("name")), code).value()));
    Put(loc, "position", MapPosition(posSrc));
    for (auto* key : {"radius", "sizeX", "sizeY", "sizeZ"})
        if (auto v = Num(l.get(key))) Put(loc, key, JNum(*v));
    return loc;
}

JsonValue MapBan(const JsonValue* raw) {
    const JsonValue& b = AsRecord(raw);
    const JsonValue* playerSource = IsNonEmptyRecord(b.get("player")) ? b.get("player") : &b;
    const JsonValue* expires = b.get("expiresAt");
    JsonValue ban = JObj();
    Put(ban, "player", MapPlayer(playerSource));
    Put(ban, "reason", JStr(Str(b.get("reason")).value_or("")));
    if (expires && expires->type == JsonValue::String && !expires->str.empty())
        Put(ban, "expiresAt", JStr(expires->str));
    else if (expires && expires->type == JsonValue::Number)
        Put(ban, "expiresAt", JStr(FormatIsoMs((int64_t)expires->num)));
    else
        Put(ban, "expiresAt", JNull());
    return ban;
}

std::optional<MappedEvent> MapPluginEvent(const std::string& type, const JsonValue* data) {
    if (!IsGameEventType(type)) return std::nullopt;
    const JsonValue& d = AsRecord(data);
    auto withPlayer = [&]() {
        const JsonValue* source = IsNonEmptyRecord(d.get("player")) ? d.get("player") : &d;
        JsonValue out = JObj();
        Put(out, "player", MapPlayer(source));
        return out;
    };
    MappedEvent ev{type, JObj()};
    if (type == "player-connected" || type == "player-disconnected") {
        ev.data = withPlayer();
    } else if (type == "chat-message") {
        auto msg = Or(Or(Str(d.get("msg")), Str(d.get("message"))), Str(d.get("text")));
        Put(ev.data, "msg", JStr(msg.value_or("")));
        Put(ev.data, "channel", JStr(MapChannel(d.get("channel"))));
        if (IsNonEmptyRecord(d.get("player"))) Put(ev.data, "player", MapPlayer(d.get("player")));
    } else if (type == "player-death") {
        ev.data = withPlayer();
        if (IsNonEmptyRecord(d.get("attacker"))) {
            try {
                Put(ev.data, "attacker", MapPlayer(d.get("attacker")));
            } catch (const NativeError&) {
                // non-player attacker
            }
        }
        if (Truthy(d.get("position"))) Put(ev.data, "position", MapPosition(d.get("position")));
        // Takaro's EventPlayerDeath only has a player `attacker`; a creature/NPC killer goes into `msg`.
        // The plugin sends the creature's display name beside its template code; older plugins send the code only.
        auto killer = Or(Or(Str(d.get("killerEntityName")), Str(d.get("killerEntity"))),
                         Str(AsRecord(d.get("killer")).get("code")));
        if (!Has(ev.data, "attacker") && killer) {
            std::string who = ev.data.get("player")->get("name")->str;
            Put(ev.data, "msg", JStr(who + " was killed by " + *killer));
        }
    } else if (type == "entity-killed") {
        ev.data = withPlayer();
        Put(ev.data, "entity", JStr(Or(Str(d.get("entity")), Str(AsRecord(d.get("entity")).get("code"))).value_or("unknown")));
        Put(ev.data, "weapon", JStr(Str(d.get("weapon")).value_or("")));
    } else {  // log
        auto msg = Or(Or(Str(d.get("msg")), Str(d.get("message"))), Str(d.get("line")));
        if (msg) Put(ev.data, "msg", JStr(*msg));
        else if (data && data->type == JsonValue::String) Put(ev.data, "msg", JStr(data->str));
        else if (data) Put(ev.data, "msg", JStr(JsonDump(*data)));
        // no data at all: JSON.stringify(undefined) is undefined, so the sidecar sent {} here
    }
    return ev;
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
    for (const JsonValue* s : sources) {
        auto id = Or(Or(Str(s->get("gameId")), Str(s->get("steamId"))), Str(s->get("platformId")));
        if (id) return *id;
    }
    Fail("Expected player identifier (gameId, or player.gameId)");
}

std::string StripSteamPrefix(const std::string& id) {
    if (id.size() >= 6 && Lower(id.substr(0, 6)) == "steam:") return id.substr(6);
    return id;
}

const std::vector<JsonValue>& ListOf(const JsonValue& value) {
    static const std::vector<JsonValue> empty;
    if (value.type == JsonValue::Array) return value.arr;
    const JsonValue& rec = AsRecord(&value);
    for (auto* key : {"items", "entities", "locations", "bans", "data"}) {
        const JsonValue* v = rec.get(key);
        if (v && v->type == JsonValue::Array) return v->arr;
    }
    return empty;
}

bool ParseLogEventsMode(const std::string& text, LogEventsMode& out) {
    std::string t = Lower(text.empty() ? "filtered" : text);
    if (t == "all") out = LogEventsMode::All;
    else if (t == "filtered") out = LogEventsMode::Filtered;
    else if (t == "none") out = LogEventsMode::None;
    else return false;
    return true;
}

bool ParseLogTailMode(const std::string& text, LogTailMode& out) {
    std::string t = Lower(text.empty() ? "auto" : text);
    if (t == "auto") out = LogTailMode::Auto;
    else if (t == "always") out = LogTailMode::Always;
    else if (t == "never") out = LogTailMode::Never;
    else return false;
    return true;
}

bool ShouldForwardLog(LogEventsMode mode, const JsonValue& data) {
    if (mode == LogEventsMode::None) return false;
    if (mode == LogEventsMode::All) return true;
    // Periodic/spammy server lines. Takaro rate-limits `log` per server (sustained 50 per 30 s), and the server
    // prints a multi-line stats block every 30 s plus bursts like "Could not prune enough replication states".
    static const std::regex kNoisy[] = {
        std::regex("^-{5,}"),
        std::regex("^Machines:$"),
        std::regex("^\\s+m#\\d+"),
        std::regex("^\\[ecss\\] Stats:"),
        std::regex("^\\[Water\\] "),
        std::regex("^Could not prune enough replication states"),
        std::regex("^\\s*$"),
    };
    const JsonValue* m = data.get("msg");
    std::string msg = m && m->type == JsonValue::String ? m->str : "";
    for (auto& re : kNoisy)
        if (std::regex_search(msg, re)) return false;
    return true;
}

bool ShouldTailLog(LogTailMode mode, const JsonValue* health) {
    if (mode == LogTailMode::Always) return true;
    if (mode == LogTailMode::Never) return false;
    if (!health) return true;
    const JsonValue* st = health->get("status");
    std::string status = Lower(st && st->type != JsonValue::Null ? JsString(st) : "");
    if (status != "ok" && status != "degraded") return true;
    const JsonValue& caps = AsRecord(health->get("capabilities"));
    for (auto* name : {"logEvents", "players"}) {
        const JsonValue* c = caps.get(name);
        if (c && !(c->type == JsonValue::String && c->str == "ok")) return true;
    }
    return false;
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

}  // namespace native
