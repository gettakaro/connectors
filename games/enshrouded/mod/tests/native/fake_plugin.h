// C++ port of the sidecar's MockPlugin (sidecar/src/testing/mockPlugin.ts) behind native::GameApi, plus the
// real plugin's /events shape and knobs the bridge tests need (delays, event ring, capability sink).
#pragma once
#include "native/adapter.h"
#include "native/game_api.h"
#include "testlib.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

namespace t {

struct RecordedRequest {
    std::string method, path, query;
    JsonValue body;
    bool hasBody = false;
};

class FakePlugin : public native::GameApi {
public:
    FakePlugin() { Reset(); }

    void Reset() {
        std::lock_guard<std::mutex> g(mu);
        health = J(R"({"status":"ok","version":"0.1.0-mock","gameBuild":"1024233","capabilities":{"logEvents":"ok","players":"ok","gameThread":"ok","chatEvents":"unimplemented"}})");
        players = J(R"J([
          {"gameId":"76561198000005875","name":"Limon","steamId":"76561198000005875","peerId":"0(1)","group":"Admins","online":true,"position":{"x":10.5,"y":20,"z":-3}},
          {"gameId":"76561198000001111","name":"Guest","steamId":"76561198000001111","peerId":"0(2)","group":"Guests","online":true}])J");
        locations = J(R"({"76561198000005875":{"x":10.5,"y":20,"z":-3},"76561198000001111":{"x":1,"y":2,"z":3}})");
        inventories = J(R"({"76561198000005875":[{"code":"Wood","name":"Wood Log","amount":25},{"code":"Sword_Iron","name":"Iron Sword","amount":1,"quality":3}]})");
        items = J(R"([{"code":"Wood","name":"Wood Log","description":"Basic material"},{"code":"Torch","name":"Torch"}])");
        entities = J(R"([{"code":"Scavenger","name":"Scavenger","type":"enemy"},{"code":"Blacksmith","name":"Blacksmith","type":"npc"},{"code":"Wolf","name":"Wolf","type":"animal"}])");
        locationsList = J(R"([{"code":"cradle","name":"Cradle","position":{"x":0,"y":100,"z":0},"radius":50}])");
        bans = J("[]");
        commandResult = JsonValue();
        unimplemented.clear();
        requests.clear();
        delays.clear();
        ring.clear();
        seq = 0;
        bootId = "boot-a";
        capabilities.clear();
    }

    // ---- GameApi ----
    native::GameResponse Call(const std::string& method, const std::string& target, const std::string& body) override {
        std::string path = target.substr(0, target.find('?'));
        std::string query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
        std::string key = method + " " + RouteKey(path);
        int delay = 0;
        {
            std::lock_guard<std::mutex> g(mu);
            RecordedRequest r;
            r.method = method;
            r.path = path;
            r.query = query;
            if (!body.empty() && native::ParseJson(body, r.body)) r.hasBody = true;
            if (path != "/events") requests.push_back(r);
            calls[key]++;
            auto d = delays.find(key);
            if (d != delays.end()) delay = d->second;
        }
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        std::unique_lock<std::mutex> g(mu);
        if (gated.count(key)) {
            waiting++;
            gateCv.wait_for(g, std::chrono::seconds(10), [&] { return !gated.count(key); });
            waiting--;
        }
        return Route(method, path, query, body, key);
    }
    void SetCapability(const std::string& name, const std::string& status, const std::string& detail) override {
        std::lock_guard<std::mutex> g(mu);
        capabilities[name] = status + "|" + detail;
    }
    std::string BaseDir() override { return baseDir; }

    // ---- helpers ----
    bool LastRequest(const std::string& method, const std::string& path, RecordedRequest& out) {
        std::lock_guard<std::mutex> g(mu);
        for (auto it = requests.rbegin(); it != requests.rend(); ++it)
            if (it->method == method && it->path == path) return out = *it, true;
        return false;
    }
    int CallCount(const std::string& key) {
        std::lock_guard<std::mutex> g(mu);
        return calls[key];
    }
    void PushEvent(const std::string& type, const std::string& dataJson) {
        std::lock_guard<std::mutex> g(mu);
        ring.push_back({++seq, type, dataJson});
        while (ring.size() > 5000) ring.erase(ring.begin());
    }
    void NewBoot(const std::string& id) {
        std::lock_guard<std::mutex> g(mu);
        bootId = id;
        ring.clear();
        seq = 0;
    }
    std::string Capability(const std::string& name) {
        std::lock_guard<std::mutex> g(mu);
        return capabilities.count(name) ? capabilities[name] : "";
    }

    // Holds every call to `key` until Release(key).
    void Gate(const std::string& key) {
        std::lock_guard<std::mutex> g(mu);
        gated.insert(key);
    }
    void Release(const std::string& key) {
        {
            std::lock_guard<std::mutex> g(mu);
            gated.erase(key);
        }
        gateCv.notify_all();
    }
    int Waiting() {
        std::lock_guard<std::mutex> g(mu);
        return waiting;
    }

    std::mutex mu;
    std::condition_variable gateCv;
    std::set<std::string> gated;
    int waiting = 0;
    JsonValue health, players, locations, inventories, items, entities, locationsList, bans, commandResult;
    std::set<std::string> unimplemented;
    std::vector<RecordedRequest> requests;
    std::map<std::string, int> delays, calls;
    std::map<std::string, std::string> capabilities;
    std::string baseDir = "/tmp";
    struct Ev {
        uint64_t seq;
        std::string type, data;
    };
    std::vector<Ev> ring;
    uint64_t seq = 0;
    std::string bootId;

private:
    static std::string RouteKey(const std::string& path) {
        if (path.compare(0, 9, "/players/") == 0) {
            size_t e = path.find('/', 9);
            return "/players/:id" + (e == std::string::npos ? "" : path.substr(e));
        }
        return path;
    }
    static std::string Decode(const std::string& s) {
        std::string o;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '%' && i + 2 < s.size()) {
                o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
                i += 2;
            } else {
                o += s[i];
            }
        }
        return o;
    }
    static native::GameResponse Json(int status, const JsonValue& v) { return {status, native::JsonDump(v)}; }
    static native::GameResponse Err(int status, const std::string& msg) {
        return {status, "{\"error\":" + JsonStr(msg) + "}"};
    }
    JsonValue* FindPlayer(const std::string& id, bool bySteam) {
        for (auto& p : players.arr) {
            const JsonValue* g = p.get("gameId");
            const JsonValue* s = p.get("steamId");
            if ((g && g->type == JsonValue::String && g->str == id) ||
                (bySteam && s && s->type == JsonValue::String && s->str == id))
                return &p;
        }
        return nullptr;
    }
    static std::string QueryParam(const std::string& q, const std::string& name) {
        size_t pos = 0;
        while (pos <= q.size()) {
            size_t amp = q.find('&', pos);
            std::string kv = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
            size_t eq = kv.find('=');
            if (kv.substr(0, eq) == name) return eq == std::string::npos ? "" : kv.substr(eq + 1);
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
        return "";
    }

    native::GameResponse Route(const std::string& method, const std::string& path, const std::string& query,
                               const std::string& rawBody, const std::string& key) {
        if (unimplemented.count(key)) return Err(501, "not implemented in this build");
        JsonValue body;
        bool hasBody = !rawBody.empty() && native::ParseJson(rawBody, body);
        auto bstr = [&](const char* k) -> std::string {
            const JsonValue* v = hasBody ? body.get(k) : nullptr;
            return v && v->type == JsonValue::String ? v->str : "";
        };
        if (method == "GET" && path.compare(0, 9, "/players/") == 0) {
            std::string rest = path.substr(9);
            size_t sl = rest.find('/');
            std::string id = Decode(rest.substr(0, sl));
            std::string sub = sl == std::string::npos ? "" : rest.substr(sl);
            if (sub == "" || sub == "/location" || sub == "/inventory") {
                JsonValue* p = FindPlayer(id, true);
                if (!p) return Err(404, "player not found");
                std::string gid = p->get("gameId")->str;
                if (sub == "/location") {
                    const JsonValue* l = locations.get(gid);
                    if (!l) l = p->get("position");
                    return l ? Json(200, *l) : native::GameResponse{200, ""};
                }
                if (sub == "/inventory") {
                    const JsonValue* inv = inventories.get(gid);
                    return inv ? Json(200, *inv) : native::GameResponse{200, "[]"};
                }
                return Json(200, *p);
            }
        }
        std::string k = method + " " + path;
        if (k == "GET /health") return Json(200, health);
        if (k == "GET /players") return Json(200, players);
        if (k == "GET /events") {
            uint64_t since = strtoull(QueryParam(query, "since").c_str(), nullptr, 10);
            std::string ls = QueryParam(query, "limit");
            size_t limit = ls.empty() ? 5000 : (size_t)strtoull(ls.c_str(), nullptr, 10);
            std::string items;
            size_t n = 0;
            uint64_t last = since;
            for (auto& e : ring) {
                if (e.seq <= since) continue;
                if (n >= limit) break;
                items += std::string(n ? "," : "") + "{\"seq\":" + std::to_string(e.seq) + ",\"type\":" + JsonStr(e.type) +
                         ",\"data\":" + e.data + ",\"ts\":\"2026-09-29T00:00:00.000Z\"}";
                last = e.seq;
                n++;
            }
            uint64_t oldest = ring.empty() ? seq + 1 : ring.front().seq;
            bool truncated = since + 1 < oldest && since < seq;
            return {200, "{\"bootId\":" + JsonStr(bootId) + ",\"seq\":" + std::to_string(n ? last : since) +
                             ",\"latestSeq\":" + std::to_string(seq) + ",\"truncated\":" + (truncated ? "true" : "false") +
                             ",\"events\":[" + items + "]}"};
        }
        if (k == "POST /message") {
            if (bstr("text").empty()) return Err(400, "text required");
            return {200, "{\"ok\":true}"};
        }
        if (k == "POST /teleport" || k == "POST /give" || k == "POST /kick") {
            JsonValue* p = FindPlayer(bstr("gameId"), false);
            if (!p) return Err(404, "player not online");
            std::string gid = p->get("gameId")->str;
            if (path == "/teleport") {
                JsonValue pos = native::JObj();
                native::Put(pos, "x", *body.get("x"));
                native::Put(pos, "y", *body.get("y"));
                native::Put(pos, "z", *body.get("z"));
                native::Put(locations, gid, pos);
            }
            if (path == "/kick") native::Put(*p, "online", native::JBool(false));
            return {200, "{\"ok\":true}"};
        }
        if (k == "POST /ban") {
            std::string id = bstr("gameId");
            JsonValue* p = FindPlayer(id, true);
            JsonValue row = native::JObj();
            native::Put(row, "gameId", native::JStr(p ? p->get("gameId")->str : id));
            native::Put(row, "steamId", native::JStr(p && p->get("steamId") ? p->get("steamId")->str : id));
            native::Put(row, "name", native::JStr(p ? p->get("name")->str : id));
            if (hasBody && body.get("reason")) native::Put(row, "reason", *body.get("reason"));
            const JsonValue* ex = hasBody ? body.get("expiresAt") : nullptr;
            native::Put(row, "expiresAt", ex && ex->type != JsonValue::Null ? *ex : native::JNull());
            bans.arr.push_back(row);
            return {200, "{\"ok\":true}"};
        }
        if (k == "POST /unban") {
            std::string id = bstr("gameId");
            std::vector<JsonValue> kept;
            for (auto& b : bans.arr) {
                const JsonValue* g = b.get("gameId");
                const JsonValue* s = b.get("steamId");
                bool match = (g && g->type == JsonValue::String && g->str == id) || (s && s->type == JsonValue::String && s->str == id);
                if (!match) kept.push_back(b);
            }
            bans.arr = kept;
            return {200, "{\"ok\":true}"};
        }
        if (k == "GET /bans") return Json(200, bans);
        if (k == "GET /items") return Json(200, items);
        if (k == "GET /entities") return Json(200, entities);
        if (k == "GET /locations") return Json(200, locationsList);
        if (k == "POST /command") {
            if (commandResult.type == JsonValue::Object) return Json(200, commandResult);
            const JsonValue* c = hasBody ? body.get("command") : nullptr;
            std::string cmd = c && c->type == JsonValue::String ? c->str : "";
            return {200, "{\"success\":true,\"output\":" + JsonStr("ran " + cmd) + "}"};
        }
        if (k == "POST /shutdown") return {200, "{\"ok\":true}"};
        return Err(404, "no route " + method + " " + path);
    }
};

}  // namespace t
