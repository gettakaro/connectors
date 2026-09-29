#include "native/adapter.h"

#include "native/json_util.h"
#include "native/mapping.h"

#include <cstdio>
#include <cstring>

namespace native {

std::string EncodeUriComponent(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", c)) {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

namespace {

std::string Truncate(const std::string& v) { return v.size() > 300 ? v.substr(0, 300) + "..." : v; }

std::string ErrorText(const std::string& raw) {
    JsonValue parsed;
    if (ParseJson(raw, parsed) && parsed.type == JsonValue::Object) {
        const JsonValue* e = parsed.get("error");
        if (e && e->type == JsonValue::String) return e->str;
        const JsonValue* m = parsed.get("message");
        if (m && m->type == JsonValue::String) return m->str;
    }
    return raw;
}

// pluginClient.ts request(): status 501 -> PluginUnimplementedError, other non-2xx -> PluginHttpError,
// empty body -> {}, invalid JSON -> PluginHttpError("invalid JSON: ...").
class PluginClient {
public:
    explicit PluginClient(GameApi& api) : api_(api) {}

    JsonValue Request(const std::string& method, const std::string& path, const std::string& body = std::string()) {
        GameResponse r = api_.Call(method, path, body);
        std::string cleanPath = path.substr(0, path.find('?'));
        if (r.status == 0)
            throw NativeError(ErrorKind::PluginUnreachable,
                              "Enshrouded plugin unreachable at in-process" + path + ": " + r.body);
        if (r.status == 501) {
            std::string t = ErrorText(r.body);
            throw NativeError(ErrorKind::PluginUnimplemented,
                              "Enshrouded plugin has not implemented " + cleanPath + " (HTTP 501)" +
                                  (t.empty() ? "" : ": " + Truncate(t)),
                              501);
        }
        if (r.status < 200 || r.status > 299) {
            std::string t = ErrorText(r.body);
            throw NativeError(ErrorKind::PluginHttp,
                              "Enshrouded plugin HTTP " + std::to_string(r.status) + " for " + cleanPath +
                                  (t.empty() ? "" : ": " + Truncate(t)),
                              r.status);
        }
        if (r.body.empty()) return JObj();
        JsonValue v;
        if (!ParseJson(r.body, v))
            throw NativeError(ErrorKind::PluginHttp,
                              "Enshrouded plugin HTTP " + std::to_string(r.status) + " for " + cleanPath +
                                  ": invalid JSON: " + Truncate(r.body),
                              r.status);
        return v;
    }

    JsonValue Players() { return Request("GET", "/players"); }
    JsonValue Player(const std::string& id) { return Request("GET", "/players/" + EncodeUriComponent(id)); }
    JsonValue Post(const std::string& path, const std::string& body) { return Request("POST", path, body); }

private:
    GameApi& api_;
};

class Adapter {
public:
    Adapter(GameApi& game, const ActionView& view, int64_t nowMs, ActionOutcome& out)
        : plugin_(game), view_(view), nowMs_(nowMs), out_(out) {}

    JsonValue Dispatch(const std::string& action, const JsonValue& args) {
        if (action == "testReachability") return TestReachability();
        if (action == "getPlayers") {
            JsonValue players = plugin_.Players();
            JsonValue list = JArr();
            if (players.type == JsonValue::Array)
                for (auto& p : players.arr) {
                    const JsonValue* online = p.get("online");
                    if (online && online->type == JsonValue::Bool && !online->b) continue;
                    list.arr.push_back(MapPlayer(&p));
                }
            out_.seenPlayers = list.arr;
            return list;
        }
        if (action == "getPlayer") {
            std::string id = PlayerId(args);
            JsonValue found;
            if (FindPlayer(id, found)) {
                JsonValue p = MapPlayer(&found);
                out_.seenPlayers.push_back(p);
                return p;
            }
            try {
                JsonValue raw = plugin_.Player(StripSteamPrefix(id));
                JsonValue p = MapPlayer(&raw);
                out_.seenPlayers.push_back(p);
                return p;
            } catch (const NativeError& e) {
                if (e.kind != ErrorKind::PluginHttp || e.status != 404) throw;
            }
            // Offline: a player this connector has seen answers as a real player (new in the native connector;
            // the sidecar answered {} for every offline player). Unknown ids still answer {} as before.
            JsonValue known;
            if (FindKnown(id, known)) return known;
            return JNull();
        }
        if (action == "getPlayerLocation") {
            std::string pid = ResolvePluginId(PlayerId(args));
            JsonValue pos = plugin_.Request("GET", "/players/" + EncodeUriComponent(pid) + "/location");
            return MapPosition(&pos);
        }
        if (action == "getPlayerInventory") {
            std::string pid = ResolvePluginId(PlayerId(args));
            JsonValue items = plugin_.Request("GET", "/players/" + EncodeUriComponent(pid) + "/inventory");
            JsonValue list = JArr();
            if (items.type == JsonValue::Array)
                for (auto& i : items.arr) list.arr.push_back(MapInventoryItem(&i));
            return list;
        }
        if (action == "giveItem") {
            std::string pid = ResolvePluginId(PlayerId(args));
            auto code = Str(args.get("item"));
            if (!code) code = Str(args.get("itemCode"));
            if (!code) code = Str(args.get("code"));
            if (!code) code = Str(AsRecord(args.get("item")).get("code"));
            if (!code) Fail("giveItem requires 'item'");
            auto amount = Num(args.get("amount"));
            if (!amount) amount = Num(args.get("quantity"));
            double n = amount.value_or(1);
            if (n <= 0) Fail("giveItem amount must be positive");
            ObjBuilder b;
            b.S("gameId", pid).S("code", *code).N("amount", n);
            const JsonValue* q = args.get("quality");
            if (q && q->type != JsonValue::Null && !(q->type == JsonValue::String && q->str.empty()))
                b.S("quality", JsString(q));
            plugin_.Post("/give", b.Done());
            return JObj();
        }
        if (action == "listItems") return MapList(plugin_.Request("GET", "/items"), MapItemDefinition);
        if (action == "listEntities") return MapList(plugin_.Request("GET", "/entities"), MapEntity);
        if (action == "listLocations") return MapList(plugin_.Request("GET", "/locations"), MapLocation);
        if (action == "executeConsoleCommand") {
            auto command = Str(args.get("command"));
            if (!command) Fail("executeConsoleCommand requires 'command'");
            JsonValue result = plugin_.Post("/command", ObjBuilder().S("command", *command).Done());
            const JsonValue* s = result.type == JsonValue::Object ? result.get("success") : nullptr;
            bool success = !(s && s->type == JsonValue::Bool && !s->b);
            const JsonValue* o = result.type == JsonValue::Object ? result.get("output") : nullptr;
            std::string output = o && o->type == JsonValue::String ? o->str : "";
            JsonValue r = JObj();
            Put(r, "success", JBool(success));
            Put(r, "rawResult", JStr(output));
            Put(r, "errorMessage", success ? JNull() : JStr(output.empty() ? "Command failed" : output));
            return r;
        }
        if (action == "sendMessage") {
            auto message = Str(args.get("message"));
            if (!message) Fail("sendMessage requires 'message'");
            const JsonValue& opts = AsRecord(args.get("opts"));
            auto sender = Str(opts.get("senderNameOverride"));
            std::string text = sender ? *sender + ": " + *message : *message;
            const JsonValue& recipient = AsRecord(opts.get("recipient"));
            auto rid = Str(recipient.get("gameId"));
            if (!rid) rid = Str(recipient.get("steamId"));
            if (!rid) rid = Str(args.get("recipientGameId"));
            ObjBuilder b;
            b.S("text", text);
            if (rid) b.S("recipientGameId", ResolvePluginId(*rid));
            plugin_.Post("/message", b.Done());
            JsonValue r = JObj();
            Put(r, "success", JBool(true));
            return r;
        }
        if (action == "teleportPlayer") {
            std::string pid = ResolvePluginId(PlayerId(args));
            auto x = Num(args.get("x")), y = Num(args.get("y")), z = Num(args.get("z"));
            if (!x || !y || !z) Fail("teleportPlayer requires numeric x, y, z");
            plugin_.Post("/teleport", ObjBuilder().S("gameId", pid).N("x", *x).N("y", *y).N("z", *z).Done());
            return JObj();
        }
        if (action == "kickPlayer") {
            std::string pid = ResolvePluginId(PlayerId(args));
            ObjBuilder b;
            b.S("gameId", pid);
            if (auto reason = Str(args.get("reason"))) b.S("reason", *reason);
            plugin_.Post("/kick", b.Done());
            return JObj();
        }
        if (action == "banPlayer") return Ban(args);
        if (action == "unbanPlayer") {
            std::string pid = ResolvePluginId(PlayerId(args));
            plugin_.Post("/unban", ObjBuilder().S("gameId", pid).Done());
            out_.ban.op = BanChange::Remove;
            out_.ban.ban.gameId = pid;
            return JObj();
        }
        if (action == "listBans") {
            if (!view_.banStoreError.empty())
                Fail("listBans refused: the timed-ban store is unreadable (" + view_.banStoreError +
                     "); fix or remove it so expiries are not reported as permanent");
            JsonValue list = MapList(plugin_.Request("GET", "/bans"), MapBan);
            // Timed bans are enforced by this connector (the game only knows permanent bans): report their expiry.
            for (auto& b : list.arr) {
                const JsonValue* id = b.get("player") ? b.get("player")->get("gameId") : nullptr;
                if (!id || id->type != JsonValue::String) continue;
                auto it = view_.timedBans.find(id->str);
                if (it == view_.timedBans.end()) continue;
                Put(b, "expiresAt", JStr(it->second.expiresAt));
                const JsonValue* r = b.get("reason");
                if (r && r->type == JsonValue::String && r->str.empty() && !it->second.reason.empty())
                    Put(b, "reason", JStr(it->second.reason));
            }
            return list;
        }
        if (action == "shutdown") {
            plugin_.Post("/shutdown", "{}");
            return JObj();
        }
        Fail("Unknown Takaro action '" + action + "'");
    }

private:
    JsonValue Ban(const JsonValue& args) {
        std::string pid = ResolvePluginId(PlayerId(args));
        auto reason = Str(args.get("reason"));
        auto expiresAt = Str(args.get("expiresAt"));
        int64_t expiresMs = 0;
        if (expiresAt) {
            if (!ParseIsoMs(*expiresAt, expiresMs))
                Fail("banPlayer expiresAt '" + *expiresAt + "' is not an ISO-8601 date with a time zone");
            if (!view_.banStoreError.empty())
                Fail("banPlayer refused: a timed ban needs the timed-ban store, which is unreadable (" +
                     view_.banStoreError + ")");
        }
        ObjBuilder b;
        b.S("gameId", pid);
        if (reason) b.S("reason", *reason);
        if (expiresAt) b.S("expiresAt", *expiresAt);
        plugin_.Post("/ban", b.Done());
        out_.ban.ban.gameId = pid;
        if (expiresAt) {
            out_.ban.op = BanChange::Upsert;
            out_.ban.ban.expiresAt = *expiresAt;
            out_.ban.ban.expiresAtMs = expiresMs;
            out_.ban.ban.reason = reason.value_or("");
            out_.ban.ban.createdAtMs = nowMs_;
        } else {
            out_.ban.op = BanChange::Remove;  // a permanent ban replaces any earlier timed one
        }
        return JObj();
    }

    JsonValue TestReachability() {
        JsonValue health;
        try {
            health = plugin_.Request("GET", "/health");
        } catch (const std::exception& e) {
            JsonValue r = JObj();
            Put(r, "connectable", JBool(false));
            Put(r, "reason", JStr(e.what()));
            return r;
        }
        const JsonValue* st = health.get("status");
        std::string status = Lower(st && st->type != JsonValue::Null ? JsString(st) : "");
        std::string caps;
        const JsonValue& capRec = AsRecord(health.get("capabilities"));
        for (auto& kv : capRec.obj) {
            bool fine = kv.second.type == JsonValue::String && (kv.second.str == "ok" || kv.second.str == "unimplemented");
            if (fine) continue;
            caps += (caps.empty() ? "" : ", ") + kv.first + "=" + JsString(&kv.second);
        }
        std::string capText = caps.empty() ? "" : "capabilities not ok: " + caps;
        JsonValue r = JObj();
        if (status == "ok") {
            Put(r, "connectable", JBool(true));
            Put(r, "reason", capText.empty() ? JNull() : JStr(capText));
        } else if (status == "degraded") {
            Put(r, "connectable", JBool(true));
            Put(r, "reason", JStr("Enshrouded plugin degraded" + (capText.empty() ? "" : "; " + capText)));
        } else {
            Put(r, "connectable", JBool(false));
            Put(r, "reason", JStr("Enshrouded plugin status '" + JsString(st) + "'" + (capText.empty() ? "" : "; " + capText)));
        }
        return r;
    }

    // Finds a plugin player by Takaro gameId (SteamID), platformId, plugin gameId, or name.
    bool FindPlayer(const std::string& id, JsonValue& out) {
        std::string needle = Lower(StripSteamPrefix(id));
        JsonValue players = plugin_.Players();
        if (players.type != JsonValue::Array) return false;
        auto field = [](const JsonValue& p, const char* k) -> const std::string* {
            const JsonValue* v = p.get(k);
            return v && v->type == JsonValue::String ? &v->str : nullptr;
        };
        for (auto* key : {"steamId", "gameId"})
            for (auto& p : players.arr) {
                const std::string* v = field(p, key);
                if (v && Lower(*v) == needle) return out = p, true;
            }
        for (auto& p : players.arr) {
            const std::string* v = field(p, "name");
            if (v && Lower(*v) == Lower(id)) return out = p, true;
        }
        return false;
    }

    bool FindKnown(const std::string& id, JsonValue& out) const {
        std::string needle = Lower(StripSteamPrefix(id));
        for (auto* key : {"gameId", "steamId"})
            for (auto& p : view_.knownPlayers) {
                const JsonValue* v = p.get(key);
                if (v && v->type == JsonValue::String && Lower(v->str) == needle) return out = p, true;
            }
        for (auto& p : view_.knownPlayers) {
            const JsonValue* v = p.get("name");
            if (v && v->type == JsonValue::String && Lower(v->str) == Lower(id)) return out = p, true;
        }
        return false;
    }

    // Plugin endpoints take the plugin's gameId; offline players (ban/unban) fall back to the SteamID as given.
    std::string ResolvePluginId(const std::string& id) {
        JsonValue found;
        if (FindPlayer(id, found)) {
            const JsonValue* g = found.get("gameId");
            if (g && g->type == JsonValue::String) return g->str;
            if (g && g->type == JsonValue::Number) return JsString(g);
        }
        return StripSteamPrefix(id);
    }

    static JsonValue MapList(const JsonValue& value, JsonValue (*map)(const JsonValue*)) {
        JsonValue list = JArr();
        for (auto& v : ListOf(value)) list.arr.push_back(map(&v));
        return list;
    }

    PluginClient plugin_;
    const ActionView& view_;
    int64_t nowMs_;
    ActionOutcome& out_;
};

}  // namespace

ActionOutcome ExecuteAction(GameApi& game, const std::string& action, const JsonValue& args, const ActionView& view,
                            int64_t nowMs) {
    ActionOutcome out;
    try {
        Adapter a(game, view, nowMs, out);
        out.payload = a.Dispatch(action, args);
        out.ok = true;
    } catch (const NativeError& e) {
        out.ok = false;
        out.ban = BanChange{};
        out.error = e.kind == ErrorKind::PluginUnimplemented
                        ? "Enshrouded connector cannot perform '" + action + "': " + e.what()
                        : e.what();
    } catch (const std::exception& e) {
        out.ok = false;
        out.ban = BanChange{};
        out.error = e.what();
    }
    return out;
}

ActionOutcome ExpireTimedBan(GameApi& game, const std::string& gameId) {
    ActionOutcome out;
    try {
        PluginClient plugin(game);
        plugin.Post("/unban", ObjBuilder().S("gameId", gameId).Done());
        out.ok = true;
        out.payload = JObj();
        out.ban.op = BanChange::Remove;
        out.ban.ban.gameId = gameId;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

}  // namespace native
