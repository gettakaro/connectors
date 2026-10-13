// Bridge behaviour behind a loopback transport and a fake plugin: identify, heartbeat, request routing and
// its adversarial edges, outbox delivery/confirmation across reconnects, backlog latency, timed bans,
// reconciliation, location fallback and the log-tail fallback.
#include "fake_plugin.h"
#include "fake_transport.h"
#include "native/bridge.h"
#include "native/fileio.h"
#include "native/mapping.h"
#include "testlib.h"

#include <atomic>
#include <memory>
#include <regex>
#include <set>

using namespace native;

namespace {

const char* LIMON = "76561198000005875";
const char* GUEST = "76561198000001111";

struct Rig {
    t::FakePlugin fake;
    t::LoopbackTransport tr;
    std::string dir;
    std::unique_ptr<Store> store;
    std::unique_ptr<Bridge> bridge;
    std::atomic<int64_t> wall{1790000000000LL};
    BridgeOptions o;
    int n = 0;

    explicit Rig(const std::string& tag, std::string existingDir = "") {
        dir = existingDir.empty() ? t::TempDir(tag) : existingDir;
        fake.baseDir = dir;
        o.config.identityToken = "identity-secret-1";
        o.config.registrationToken = "registration-secret-2";
        o.config.serverName = "Rig";
        o.config.logTail = LogTailMode::Never;
        o.config.pollIntervalMs = 20;
        o.config.actionTimeoutMs = 5000;
        o.config.logFile = dir + "/enshrouded_server.log";
        o.game = &fake;
        o.transport = &tr;
        o.wallMs = [this] { return wall.load(); };
        o.banCheckIntervalMs = 50;
        o.healthIntervalMs = 100;
        o.reconcileIntervalMs = 200;
    }
    ~Rig() { Stop(); }
    std::string StateDir() { return dir + "/takaro/connector-state"; }
    void Start() {
        store.reset(new Store(ResolveStatePaths(dir, [](const char*) { return std::string(); })));
        o.store = store.get();
        bridge.reset(new Bridge(o));
        bridge->Start();
    }
    void Stop() {
        if (bridge) bridge->Stop();
        bridge.reset();
    }
    uint64_t Connect(bool identify = true) {
        uint64_t e = tr.Open();
        if (!identify) return e;
        tr.Inject(R"({"type":"connected","payload":{"clientId":"c1"}})");
        tr.Inject(R"({"type":"identifyResponse","payload":{"gameServerId":"gs-1"}})");
        CHECK(t::WaitFor([&] { return tr.identifiedEpoch == e; }, 2000));
        return e;
    }
    std::string Send(const std::string& action, const std::string& argsJson, std::string id = "") {
        if (id.empty()) id = "req-" + std::to_string(++n);
        tr.Inject("{\"type\":\"request\",\"requestId\":" + JsonStr(id) + ",\"payload\":{\"action\":" + JsonStr(action) +
                  ",\"args\":" + argsJson + "}}");
        return id;
    }
    // Waits for the (first) response to `id`.
    bool Response(const std::string& id, JsonValue& out, int timeoutMs = 5000) {
        return t::WaitFor(
            [&] {
                tr.Drain();
                auto f = tr.WireFrames("response", id);
                if (f.empty()) return false;
                out = f.front();
                return true;
            },
            timeoutMs);
    }
    JsonValue Call(const std::string& action, const std::string& argsJson) {
        JsonValue r;
        std::string id = Send(action, argsJson);
        if (!Response(id, r)) {
            CHECK_MSG(false, "no response to " + action);
            return JObj();
        }
        return r;
    }
    std::vector<JsonValue> Events(const std::string& type = "") {
        std::vector<JsonValue> out;
        for (auto& f : tr.WireFrames("gameEvent"))
            if (type.empty() || f.get("payload")->get("type")->str == type) out.push_back(f);
        return out;
    }
    JsonValue Health() {
        JsonValue h;
        ParseJson(bridge->HealthJson(), h);
        return h;
    }
    double HealthNum(const char* section, const char* key) {
        JsonValue h = Health();
        const JsonValue* s = section ? h.get(section) : &h;
        const JsonValue* v = s ? s->get(key) : nullptr;
        return v ? v->num : -1;
    }
};

std::string File(const std::string& p) { return t::ReadFile(p); }

void IdentifyAndHeartbeat() {
    t::Group("bridge-identify");
    Rig r("identify");
    r.o.config.protocolV1 = false;  // protocol=0: the identify is byte-for-byte the protocol 0 one
    r.Start();
    uint64_t e = r.tr.Open();
    std::vector<t::Written> w;
    CHECK(t::WaitFor([&] {
        auto d = r.tr.Drain();
        w.insert(w.end(), d.begin(), d.end());
        return !w.empty();
    }));
    CHECK(!w.empty() && w[0].kind == FrameKind::Control &&
          t::JsonTextEq(w[0].text, R"({"type":"identify","payload":{"identityToken":"identity-secret-1","registrationToken":"registration-secret-2","name":"Rig"}})"));
    r.tr.Inject(R"({"type":"identifyResponse","payload":{"gameServerId":"gs-1"}})");
    CHECK(t::WaitFor([&] { return r.tr.identifiedEpoch == e; }));
    r.tr.Inject(R"({"type":"ping"})");
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return !r.tr.WireFrames("pong").empty();
    }));
    JsonValue resp = r.Call("testReachability", "{}");
    CHECK(t::JsonTextEq(JsonDump(resp), R"({"type":"response","requestId":"req-1","payload":{"connectable":true,"reason":null}})"));
    CHECK(t::WaitFor([&] { return r.Health().get("identified") && r.Health().get("identified")->b; }));
    std::string h = r.bridge->HealthJson();
    CHECK(h.find("identity-secret-1") == std::string::npos && h.find("registration-secret-2") == std::string::npos);
    CHECK(r.fake.Capability("connectorState").rfind("ok|", 0) == 0);
}

void IdentifyError() {
    t::Group("bridge-identify-error");
    Rig r("identify-error");
    r.Start();
    uint64_t e = r.tr.Open();
    r.tr.Inject(R"({"type":"identifyResponse","payload":{"error":{"name":"BadRequestError","message":"bad token identity-secret-1","http":400}}})");
    CHECK(t::WaitFor([&] { return !r.tr.CloseRequests().empty(); }));
    CHECK(!r.tr.CloseRequests().empty() && r.tr.CloseRequests()[0] == std::to_string(e) + ":identify rejected");
    CHECK(r.tr.identifiedEpoch == 0);
    bool seen = t::WaitFor([&] {
        JsonValue h = r.Health();
        const JsonValue* v = h.get("lastIdentifyError");
        return v && v->str.find("BadRequestError") != std::string::npos;
    });
    CHECK_MSG(seen, r.bridge->HealthJson().substr(0, 400));
    CHECK(r.bridge->HealthJson().find("identity-secret-1") == std::string::npos);  // redacted
}

void ReconfigureWhileRunning() {
    t::Group("bridge-reconfigure");
    Rig r("reconfigure");
    std::mutex mu;
    std::vector<std::string> accepted, rejected;
    r.o.onIdentified = [&](const LiveSettings& s) {
        std::lock_guard<std::mutex> g(mu);
        accepted.push_back(s.registrationToken + "|" + s.serverName);
    };
    r.o.onIdentifyRejected = [&](const std::string& summary, int status) {
        std::lock_guard<std::mutex> g(mu);
        rejected.push_back(std::to_string(status) + "|" + summary);
    };
    r.Start();
    r.tr.Open();
    r.tr.Inject(R"({"type":"identifyResponse","payload":{"error":{"name":"AxiosError","message":"Request failed with status code 409","config":{"headers":{"x-takaro-token":"registration-secret-2"}},"status":409}}})");
    CHECK(t::WaitFor([&] {
        std::lock_guard<std::mutex> g(mu);
        return !rejected.empty();
    }));
    {
        std::lock_guard<std::mutex> g(mu);
        CHECK(!rejected.empty() && rejected[0] == "409|AxiosError: Request failed with status code 409 (HTTP 409)");
    }
    // A corrected token: the bridge retargets the transport, and the next connection identifies with it.
    r.bridge->Reconfigure({"wss://other:8443/", "registration-new-3", "identity-secret-1", "Rig 2"}, true);
    CHECK(t::WaitFor([&] { return !r.tr.Retargets().empty(); }));
    CHECK(!r.tr.Retargets().empty() && r.tr.Retargets()[0] == "wss://other:8443/|connect");
    r.tr.Close("connection settings changed");
    r.tr.Drain();
    uint64_t e2 = r.tr.Open();
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return !r.tr.WireFrames("identify").empty();
    }));
    auto ids = r.tr.WireFrames("identify");
    CHECK(!ids.empty() && Str(AsRecord(ids.back().get("payload")).get("registrationToken")).value_or("") == "registration-new-3");
    CHECK(!ids.empty() && Str(AsRecord(ids.back().get("payload")).get("name")).value_or("") == "Rig 2");
    r.tr.Inject(R"({"type":"identifyResponse","payload":{"gameServerId":"gs-2"}})");
    CHECK(t::WaitFor([&] { return r.tr.identifiedEpoch == e2; }));
    CHECK(t::WaitFor([&] {
        std::lock_guard<std::mutex> g(mu);
        return !accepted.empty();
    }));
    {
        std::lock_guard<std::mutex> g(mu);
        CHECK(!accepted.empty() && accepted[0] == "registration-new-3|Rig 2");
    }
    std::string h = r.bridge->HealthJson();
    CHECK(h.find("registration-secret-2") == std::string::npos && h.find("registration-new-3") == std::string::npos);
    // Token removed: the transport goes idle.
    r.bridge->Reconfigure({"wss://other:8443/", "", "identity-secret-1", "Rig 2"}, false);
    CHECK(t::WaitFor([&] { return r.tr.Retargets().size() == 2; }));
    CHECK(r.tr.Retargets().size() == 2 && r.tr.Retargets()[1] == "wss://other:8443/|idle");
}

void EventsConfirmOnLaterPong() {
    t::Group("bridge-outbox-confirm");
    Rig r("confirm");
    r.fake.PushEvent("player-connected", std::string("{\"player\":{\"gameId\":\"") + LIMON + "\",\"name\":\"Limon\",\"steamId\":\"" + LIMON + "\"}}");
    r.fake.PushEvent("chat-message", "{\"msg\":\"hi\",\"channel\":\"global\"}");
    r.fake.PushEvent("entity-killed", std::string("{\"player\":{\"gameId\":\"") + LIMON + "\",\"name\":\"Limon\"},\"entity\":\"Wolf\"}");
    r.Start();
    uint64_t e = r.tr.Open();
    // nothing is delivered before identify
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    r.tr.Drain();
    CHECK(r.Events().empty());
    r.tr.Inject(R"({"type":"identifyResponse","payload":{}})");
    CHECK(t::WaitFor([&] { return r.tr.identifiedEpoch == e; }));
    // a ping written BEFORE the events: its pong must confirm nothing
    r.tr.Ping();
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events().size() == 3;
    }));
    auto ev = r.Events();
    CHECK(ev.size() == 3 && ev[0].get("payload")->get("type")->str == "player-connected" &&
          ev[1].get("payload")->get("type")->str == "chat-message" && ev[2].get("payload")->get("type")->str == "entity-killed");
    CHECK(ev.size() == 3 && t::JsonTextEq(JsonDump(*ev[0].get("payload")->get("data")),
                                          std::string("{\"player\":{\"gameId\":\"") + LIMON + "\",\"name\":\"Limon\",\"steamId\":\"" + LIMON +
                                              "\",\"platformId\":\"steam:" + LIMON + "\"}}"));
    r.tr.Pong();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK_EQ(r.HealthNum("outbox", "pending"), 3.0);
    // a LATER ping's pong confirms all three
    r.tr.Ping();
    r.tr.Pong();
    CHECK(t::WaitFor([&] { return r.HealthNum("outbox", "pending") == 0 && r.HealthNum("outbox", "confirmedTotal") == 3; }));
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/event-outbox.json").find("\"pending\":[]") != std::string::npos; }));
    // the connect was admitted: online store updated in the sidecar's format
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/online-players.json").find(LIMON) != std::string::npos; }));
}

void ReconnectResend() {
    t::Group("bridge-reconnect");
    Rig r("reconnect");
    r.fake.PushEvent("chat-message", "{\"msg\":\"one\"}");
    r.fake.PushEvent("chat-message", "{\"msg\":\"two\"}");
    r.Start();
    r.Connect();
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events().size() == 2;
    }));
    // connection drops before any pong: both must go out again on the next epoch
    r.tr.Close("tcp abort");
    uint64_t e2 = r.tr.Open();
    std::vector<t::Written> w;
    CHECK(t::WaitFor([&] {
        auto d = r.tr.Drain();
        w.insert(w.end(), d.begin(), d.end());
        for (auto& x : w)
            if (x.text.find("\"identify\"") != std::string::npos) return true;
        return false;
    }));
    r.tr.Inject(R"({"type":"identifyResponse","payload":{}})");
    CHECK(t::WaitFor([&] { return r.tr.identifiedEpoch == e2; }));
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events().size() == 4;
    }));
    r.tr.Ping();
    r.tr.Pong();
    CHECK(t::WaitFor([&] { return r.HealthNum("outbox", "pending") == 0; }));
    // a result that completes after its connection closed is never sent on the new one
    r.fake.Gate("POST /kick");
    std::string id = r.Send("kickPlayer", std::string("{\"gameId\":\"") + LIMON + "\"}");
    CHECK(t::WaitFor([&] { return r.fake.Waiting() == 1; }));
    r.tr.Close("restart");
    uint64_t e3 = r.tr.Open();
    r.tr.Inject(R"({"type":"identifyResponse","payload":{}})");
    CHECK(t::WaitFor([&] { return r.tr.identifiedEpoch == e3; }));
    r.fake.Release("POST /kick");
    CHECK(t::WaitFor([&] { return r.HealthNum("requests", "droppedResponses") >= 1; }));
    r.tr.Drain();
    CHECK(r.tr.WireFrames("response", id).empty());
}

void BacklogLatency() {
    t::Group("bridge-backlog-latency");
    Rig r("backlog");
    for (int i = 0; i < 5000; i++) r.fake.PushEvent("chat-message", "{\"msg\":\"m" + std::to_string(i) + "\"}");
    r.Start();
    // wait until the whole backlog is in the outbox
    CHECK(t::WaitFor([&] { return r.HealthNum("outbox", "admitted") == 5000; }, 10000));
    r.Connect();
    std::string id = r.Send("testReachability", "{}");
    int64_t t0 = t::SteadyMs();
    size_t eventsBefore = 0;
    bool answered = false;
    // a slow socket: 64 frames per 5 ms
    while (t::SteadyMs() - t0 < 5000) {
        for (auto& w : r.tr.Drain(64)) {
            if (w.kind == FrameKind::Event && !answered) eventsBefore++;
            if (w.text.find(id) != std::string::npos) answered = true;
        }
        if (answered) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    int64_t latency = t::SteadyMs() - t0;
    CHECK_MSG(answered, "request not answered during replay");
    CHECK_MSG(latency < 1000, "request latency " + std::to_string(latency) + "ms during a 5000-event replay");
    CHECK_MSG(eventsBefore < 2000, std::to_string(eventsBefore) + " events went out before the response");
    printf("backlog: response after %lldms with %zu of 5000 replay events ahead of it\n", (long long)latency, eventsBefore);
    // the replay itself completes and is confirmed
    CHECK(t::WaitFor(
        [&] {
            r.tr.Drain();
            return r.Events().size() >= 5000;
        },
        15000));
    r.tr.Ping();
    r.tr.Pong();
    CHECK(t::WaitFor([&] { return r.HealthNum("outbox", "pending") == 0; }, 5000));
    CHECK_EQ(r.Events().size(), (size_t)5000);
}

void Adversarial() {
    t::Group("bridge-adversarial");
    Rig r("adversarial");
    r.Start();
    r.Connect();
    JsonValue resp;
    // duplicate requestId while the first is still running, and again after it completed
    r.fake.Gate("POST /kick");
    std::string id = r.Send("kickPlayer", std::string("{\"gameId\":\"") + LIMON + "\"}", "dup-1");
    CHECK(t::WaitFor([&] { return r.fake.Waiting() == 1; }));
    r.Send("kickPlayer", std::string("{\"gameId\":\"") + LIMON + "\"}", "dup-1");
    CHECK(r.Response("dup-1", resp) && resp.get("error") && resp.get("error")->str == "duplicate requestId");
    r.fake.Release("POST /kick");
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.tr.WireFrames("response", "dup-1").size() == 2;
    }));
    CHECK_EQ(r.fake.CallCount("POST /kick"), 1);
    r.Send("getPlayers", "{}", "dup-1");
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.tr.WireFrames("response", "dup-1").size() == 3;
    }));
    CHECK(r.tr.WireFrames("response", "dup-1").back().get("error")->str == "duplicate requestId");
    // requestId length: 128 served, 129 dropped
    std::string ok128(128, 'a'), bad129(129, 'b');
    r.Send("getPlayers", "{}", ok128);
    CHECK(r.Response(ok128, resp));
    r.Send("getPlayers", "{}", bad129);
    CHECK(!r.Response(bad129, resp, 300));
    CHECK(t::WaitFor([&] { return r.HealthNum("requests", "oversizeRequestIds") == 1; }));
    // malformed frames and nesting beyond 64 are ignored; the connection keeps serving
    r.tr.Inject("not json");
    r.tr.Inject("{\"type\":\"request\",");
    r.tr.Inject("{\"type\":\"request\",\"requestId\":\"deep\",\"payload\":" + std::string(70, '[') + std::string(70, ']') + "}");
    CHECK(!r.Response("deep", resp, 300));
    CHECK(t::WaitFor([&] { return r.HealthNum("requests", "malformedFrames") == 3; }));
    // args given as a JSON string nested too deep
    r.Send("getPlayer", JsonStr(std::string(65, '[') + std::string(65, ']')), "deep-args");
    CHECK(r.Response("deep-args", resp) && resp.get("error") &&
          resp.get("error")->str == "request args exceed JSON nesting depth 64");
    // explicit nulls in args (Takaro sends them for optional fields)
    resp = r.Call("giveItem", std::string("{\"gameId\":\"") + LIMON + "\",\"item\":\"Wood\",\"amount\":null,\"quality\":null}");
    CHECK(resp.get("payload") && !resp.get("error"));
    resp = r.Call("getPlayers", "null");  // (the kick above took Limon offline in the mock)
    CHECK(resp.get("payload") && resp.get("payload")->type == JsonValue::Array && resp.get("payload")->arr.size() == 1);
    // missing action / missing requestId
    r.tr.Inject(R"({"type":"request","requestId":"no-action","payload":{}})");
    CHECK(r.Response("no-action", resp) && resp.get("error")->str == "Takaro request missing action");
    r.tr.Inject(R"({"type":"request","payload":{"action":"getPlayers"}})");
    // unknown frame types and Takaro errors are tolerated
    r.tr.Inject(R"({"type":"error","payload":{"message":"something"}})");
    r.tr.Inject(R"({"type":"weird"})");
    resp = r.Call("testReachability", "{}");
    CHECK(resp.get("payload") && resp.get("payload")->get("connectable")->b);
}

void OverloadAndTimeouts() {
    t::Group("bridge-overload-timeouts");
    {
        Rig r("overload");
        r.o.config.actionWorkers = 4;
        r.o.config.actionTimeoutMs = 60000;
        r.Start();
        r.Connect();
        r.fake.Gate("POST /kick");
        for (int i = 0; i < 130; i++) r.Send("kickPlayer", std::string("{\"gameId\":\"") + LIMON + "\"}", "o-" + std::to_string(i));
        int overloaded = 0;
        CHECK(t::WaitFor([&] {
            r.tr.Drain();
            overloaded = 0;
            for (auto& f : r.tr.WireFrames("response"))
                if (f.get("error") && f.get("error")->str == "native action queue overloaded") overloaded++;
            return overloaded == 2;
        }));
        CHECK_EQ(overloaded, 2);
        r.fake.Release("POST /kick");
        CHECK(t::WaitFor(
            [&] {
                r.tr.Drain();
                return r.tr.WireFrames("response").size() == 130;
            },
            10000));
    }
    {
        Rig r("timeout");
        r.o.config.actionWorkers = 1;
        r.o.config.actionTimeoutMs = 300;
        r.Start();
        r.Connect();
        r.fake.Gate("POST /kick");
        std::string a = r.Send("kickPlayer", std::string("{\"gameId\":\"") + LIMON + "\"}", "slow-a");
        CHECK(t::WaitFor([&] { return r.fake.Waiting() == 1; }));
        std::string b = r.Send("kickPlayer", std::string("{\"gameId\":\"") + GUEST + "\"}", "slow-b");
        JsonValue ra, rb;
        CHECK(r.Response(a, ra, 2000) && ra.get("error") &&
              ra.get("error")->str == "Enshrouded plugin call 'kickPlayer' timed out after 300ms");
        CHECK(r.Response(b, rb, 2000) && rb.get("error") &&
              rb.get("error")->str.find("did not start 'kickPlayer'") != std::string::npos);
        r.fake.Release("POST /kick");
        CHECK(t::WaitFor([&] { return r.HealthNum("requests", "lateResults") == 1; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        r.tr.Drain();
        CHECK_EQ(r.tr.WireFrames("response", a).size(), (size_t)1);  // the late result is dropped
        CHECK_EQ(r.tr.WireFrames("response", b).size(), (size_t)1);
        CHECK_EQ(r.fake.CallCount("POST /kick"), 1);  // the unstarted job was cancelled, never ran
    }
}

void TimedBans() {
    t::Group("bridge-timed-bans");
    Rig r("timed-bans");
    r.Start();
    r.Connect();
    std::string until = FormatIsoMs(r.wall + 60000);
    JsonValue resp = r.Call("banPlayer", std::string("{\"player\":{\"gameId\":\"") + LIMON + "\"},\"reason\":\"grief\",\"expiresAt\":\"" + until + "\"}");
    CHECK(resp.get("payload") && !resp.get("error"));
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/timed-bans.json").find(LIMON) != std::string::npos; }));
    CHECK(File(r.StateDir() + "/ban-intent.json") == "[]");
    resp = r.Call("listBans", "{}");
    const JsonValue* bans = resp.get("payload");
    CHECK(bans && bans->arr.size() == 1 && bans->arr[0].get("expiresAt")->str == until &&
          bans->arr[0].get("reason")->str == "grief");
    // a second timed ban on GUEST, then made permanent: the timed record goes away
    std::string until2 = FormatIsoMs(r.wall + 120000);
    r.Call("banPlayer", std::string("{\"gameId\":\"") + GUEST + "\",\"expiresAt\":\"" + until2 + "\"}");
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/timed-bans.json").find(GUEST) != std::string::npos; }));
    r.Call("banPlayer", std::string("{\"gameId\":\"") + GUEST + "\",\"expiresAt\":null}");
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/timed-bans.json").find(GUEST) == std::string::npos; }));
    // invalid expiry is refused before anything happens in the game
    int bansBefore = r.fake.CallCount("POST /ban");
    resp = r.Call("banPlayer", std::string("{\"gameId\":\"") + LIMON + "\",\"expiresAt\":\"next tuesday\"}");
    CHECK(resp.get("error") && resp.get("error")->str.find("not an ISO-8601") != std::string::npos);
    CHECK_EQ(r.fake.CallCount("POST /ban"), bansBefore);
    // time passes: the connector lifts the expired ban itself
    CHECK_EQ(r.fake.CallCount("POST /unban"), 0);
    r.wall += 61000;
    CHECK(t::WaitFor([&] { return r.fake.CallCount("POST /unban") == 1; }));
    t::RecordedRequest req;
    CHECK(r.fake.LastRequest("POST", "/unban", req) && req.body.get("gameId")->str == LIMON);
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/timed-bans.json") == "[]"; }));
    CHECK(t::WaitFor([&] { return r.Health().get("timedBans") && r.Health().get("timedBans")->get("expired")->num == 1; }));
    resp = r.Call("listBans", "{}");
    bool limonGone = resp.get("payload") && !resp.get("payload")->arr.empty();  // GUEST's permanent ban stays
    for (auto& b : resp.get("payload") ? resp.get("payload")->arr : std::vector<JsonValue>())
        limonGone = limonGone && b.get("player")->get("gameId")->str == GUEST;
    CHECK(limonGone);
    // the expiry survives a restart
    r.Call("banPlayer", std::string("{\"gameId\":\"") + GUEST + "\",\"expiresAt\":\"" + FormatIsoMs(r.wall + 5000) + "\"}");
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/timed-bans.json").find(GUEST) != std::string::npos; }));
    r.Stop();
    r.wall += 6000;
    r.Start();
    CHECK(t::WaitFor([&] { return r.fake.CallCount("POST /unban") == 2; }));
}

void CorruptBanStore() {
    t::Group("bridge-corrupt-ban-store");
    Rig r("corrupt-bans");
    std::string err;
    EnsureDirectory(r.StateDir(), err);
    t::WriteFile(r.StateDir() + "/timed-bans.json", "[{\"gameId\":\"x\",\"expiresAt\":");
    r.Start();
    r.Connect();
    JsonValue resp = r.Call("banPlayer", std::string("{\"gameId\":\"") + LIMON + "\",\"expiresAt\":\"2030-01-01T00:00:00.000Z\"}");
    CHECK(resp.get("error") && resp.get("error")->str.find("timed-ban store") != std::string::npos);
    CHECK_EQ(r.fake.CallCount("POST /ban"), 0);
    resp = r.Call("banPlayer", std::string("{\"gameId\":\"") + LIMON + "\"}");  // permanent: allowed
    CHECK(resp.get("payload") && !resp.get("error"));
    resp = r.Call("listBans", "{}");
    CHECK(resp.get("error") && resp.get("error")->str.find("listBans refused") != std::string::npos);
    CHECK_EQ(File(r.StateDir() + "/timed-bans.json"), std::string("[{\"gameId\":\"x\",\"expiresAt\":"));  // untouched
    CHECK(t::WaitFor([&] { return r.fake.Capability("connectorState").rfind("degraded|", 0) == 0; }));
    resp = r.Call("testReachability", "{}");
    CHECK(resp.get("payload") && resp.get("payload")->get("connectable")->b);
}

void BanIntentRecovery() {
    t::Group("bridge-ban-intent-recovery");
    Rig r("intent");
    std::string err;
    EnsureDirectory(r.StateDir(), err);
    t::WriteFile(r.StateDir() + "/ban-intent.json",
                 std::string("[{\"id\":\"r1:a\",\"op\":\"ban\",\"gameId\":\"") + LIMON +
                     "\",\"expiresAt\":\"2030-01-01T00:00:00.000Z\",\"reason\":\"grief\",\"at\":1},"
                     "{\"id\":\"r2:b\",\"op\":\"unban\",\"gameId\":\"" + GUEST + "\",\"at\":2}]");
    r.Start();
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/ban-intent.json") == "[]"; }));
    std::string bans = File(r.StateDir() + "/timed-bans.json");
    CHECK(bans.find(LIMON) != std::string::npos && bans.find("2030-01-01T00:00:00.000Z") != std::string::npos);
    // the interrupted unban is finished by the expiry loop
    CHECK(t::WaitFor([&] { return r.fake.CallCount("POST /unban") == 1; }));
    t::RecordedRequest req;
    CHECK(r.fake.LastRequest("POST", "/unban", req) && req.body.get("gameId")->str == GUEST);
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/timed-bans.json").find(GUEST) == std::string::npos; }));
}

void ReconcileAndLocationFallback() {
    t::Group("bridge-reconcile");
    Rig r("reconcile");
    std::string err;
    EnsureDirectory(r.StateDir(), err);
    std::string limon = std::string("{\"gameId\":\"") + LIMON + "\",\"name\":\"Limon\",\"steamId\":\"" + LIMON +
                        "\",\"platformId\":\"steam:" + LIMON + "\"}";
    t::WriteFile(r.StateDir() + "/online-players.json", "[" + limon + "]");
    r.fake.players = t::J("[]");  // the server restarted while Limon was online
    r.Start();
    r.Connect();
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events("player-disconnected").size() == 1;
    }));
    auto ev = r.Events("player-disconnected");
    CHECK(!ev.empty() && t::JsonTextEq(JsonDump(*ev[0].get("payload")->get("data")), "{\"player\":" + limon + "}"));
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/online-players.json") == "[]"; }));
    // Takaro asks for the location right after the event: answered from the fallback window
    JsonValue resp = r.Call("getPlayerLocation", std::string("{\"gameId\":\"") + LIMON + "\"}");
    CHECK(resp.get("payload") && t::JsonTextEq(JsonDump(*resp.get("payload")), R"({"x":0,"y":0,"z":0})"));
    // other players have no window
    resp = r.Call("getPlayerLocation", "{\"gameId\":\"nobody\"}");
    CHECK(resp.get("error") && resp.get("error")->str == "Enshrouded plugin HTTP 404 for /players/nobody/location: player not found");
}

void LocationAfterLeave() {
    t::Group("bridge-location-fallback");
    Rig r("location");
    r.Start();
    r.Connect();
    JsonValue resp = r.Call("getPlayerLocation", std::string("{\"gameId\":\"") + LIMON + "\"}");
    CHECK(resp.get("payload") && t::JsonTextEq(JsonDump(*resp.get("payload")), R"({"x":10.5,"y":20,"z":-3})"));
    {
        std::lock_guard<std::mutex> g(r.fake.mu);
        r.fake.players.arr.erase(r.fake.players.arr.begin());
    }
    r.fake.PushEvent("player-disconnected", std::string("{\"player\":{\"gameId\":\"") + LIMON + "\",\"name\":\"Limon\",\"steamId\":\"" + LIMON + "\"}}");
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events("player-disconnected").size() == 1;
    }));
    resp = r.Call("getPlayerLocation", std::string("{\"gameId\":\"") + LIMON + "\"}");
    CHECK(resp.get("payload") && t::JsonTextEq(JsonDump(*resp.get("payload")), R"({"x":10.5,"y":20,"z":-3})"));
}

void WindowRules() {
    t::Group("bridge-location-window");
    Rig r("window");
    r.o.transport = &r.tr;
    r.store.reset(new Store(ResolveStatePaths(r.dir, [](const char*) { return std::string(); })));
    r.o.store = r.store.get();
    Bridge b(r.o);  // not started: exercise the window bookkeeping directly
    JsonValue player = t::J(std::string("{\"player\":{\"gameId\":\"") + LIMON + "\",\"name\":\"Limon\",\"steamId\":\"" + LIMON + "\"}}");
    JsonValue args = t::J(std::string("{\"gameId\":\"") + LIMON + "\"}");
    JsonValue fb;
    b.NoteConnectionEvent("chat-message", player, 0);
    CHECK(!b.EventLocationFallback(args, 0, fb));
    b.NoteConnectionEvent("player-disconnected", player, 1000);
    CHECK(b.EventLocationFallback(args, 1000 + Bridge::kLocationFallbackMs, fb) && t::JsonTextEq(JsonDump(fb), R"({"x":0,"y":0,"z":0})"));
    CHECK(!b.EventLocationFallback(args, 2000 + Bridge::kLocationFallbackMs, fb));
    CHECK(!b.EventLocationFallback(t::J("{\"gameId\":\"someone-else\"}"), 0, fb));
    CHECK(!b.EventLocationFallback(t::J("{}"), 0, fb));
}

void LogTailFallback() {
    t::Group("bridge-log-tail");
    Rig r("logtail");
    r.o.config.logTail = LogTailMode::Auto;
    r.fake.health = t::J(R"({"status":"ok","capabilities":{"logEvents":"degraded","players":"degraded"}})");
    t::WriteFile(r.o.config.logFile, "[I 00:00:01,000] boot\n");
    r.fake.PushEvent("player-connected", std::string("{\"player\":{\"gameId\":\"") + LIMON + "\",\"name\":\"RingName\",\"steamId\":\"" + LIMON + "\"}}");
    r.Start();
    r.Connect();
    CHECK(t::WaitFor([&] { return r.Health().get("logTail") && r.Health().get("logTail")->get("active")->b; }));
    FILE* f = fopen(r.o.config.logFile.c_str(), "ab");
    std::string lines = std::string("[I 00:03:52,650] [online] Added peer 0(1) (steamid:") + LIMON + ")\n" +
                        "[I 00:04:24,062] [server] Machine '1': Player '0(0)' logged in\n"
                        "[I 00:04:24,063] [server] Player 'Limon' logged in with Permissions:\n";
    fwrite(lines.data(), 1, lines.size(), f);
    fclose(f);
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events("player-connected").size() == 1;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    r.tr.Drain();
    auto ev = r.Events("player-connected");
    CHECK_EQ(ev.size(), (size_t)1);  // the ring's connect was suppressed while the tail owns joins
    CHECK(!ev.empty() && ev[0].get("payload")->get("data")->get("player")->get("name")->str == "Limon");
    // the hook recovers: the tail switches off
    {
        std::lock_guard<std::mutex> g(r.fake.mu);
        r.fake.health = t::J(R"({"status":"ok","capabilities":{"logEvents":"ok","players":"ok"}})");
    }
    CHECK(t::WaitFor([&] { return !r.Health().get("logTail")->get("active")->b; }));
}

void LogFilter() {
    t::Group("bridge-log-filter");
    for (int mode = 0; mode < 2; mode++) {
        Rig r(mode ? "logfilter-none" : "logfilter");
        r.o.config.logEvents = mode ? LogEventsMode::None : LogEventsMode::Filtered;
        r.fake.PushEvent("log", "{\"msg\":\"-------------- Session ----------------\",\"level\":\"info\"}");
        r.fake.PushEvent("log", "{\"msg\":\"[server] Saved\",\"level\":\"info\"}");
        r.fake.PushEvent("chat-message", "{\"msg\":\"marker\"}");
        r.Start();
        r.Connect();
        CHECK(t::WaitFor([&] {
            r.tr.Drain();
            return r.Events("chat-message").size() == 1;
        }));
        auto logs = r.Events("log");
        CHECK_EQ(logs.size(), (size_t)(mode ? 0 : 1));
        if (!mode && !logs.empty()) CHECK(t::JsonTextEq(JsonDump(*logs[0].get("payload")->get("data")), R"({"msg":"[server] Saved"})"));
    }
}

void OutboxSurvivesRestart() {
    t::Group("bridge-outbox-restart");
    Rig r("restart");
    r.fake.PushEvent("chat-message", "{\"msg\":\"durable\"}");
    r.Start();
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/event-outbox.json").find("durable") != std::string::npos; }));
    r.Stop();
    // the process died: the ring is gone, a new boot starts empty
    r.fake.NewBoot("boot-b");
    r.tr.wire.clear();
    r.Start();
    r.Connect();
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events("chat-message").size() == 1;
    }));
    r.tr.Ping();
    r.tr.Pong();
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/event-outbox.json").find("durable") == std::string::npos; }));
    CHECK(File(r.StateDir() + "/event-outbox.json").find("\"bootId\":\"boot-b\"") != std::string::npos);
}

void NewBootReconciles() {
    t::Group("bridge-new-boot");
    Rig r("newboot");
    r.Start();
    r.Connect();
    r.fake.PushEvent("player-connected", std::string("{\"player\":{\"gameId\":\"") + GUEST + "\",\"name\":\"Guest\",\"steamId\":\"" + GUEST + "\"}}");
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/online-players.json").find(GUEST) != std::string::npos; }));
    {
        std::lock_guard<std::mutex> g(r.fake.mu);
        r.fake.players = t::J("[]");
    }
    r.fake.NewBoot("boot-new");
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events("player-disconnected").size() == 1;
    }));
}

void PeriodicReconcileNeedsTwoMisses() {
    t::Group("bridge-periodic-reconcile");
    Rig r("periodic");
    r.o.reconcileIntervalMs = 150;
    r.Start();
    r.Connect();
    r.fake.PushEvent("player-connected", std::string("{\"player\":{\"gameId\":\"") + LIMON + "\",\"name\":\"Limon\",\"steamId\":\"" + LIMON + "\"}}");
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/online-players.json").find(LIMON) != std::string::npos; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // several checks while Limon is listed
    r.tr.Drain();
    CHECK(r.Events("player-disconnected").empty());
    {
        std::lock_guard<std::mutex> g(r.fake.mu);
        r.fake.players = t::J("[]");  // left without a leave line reaching the ring
    }
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events("player-disconnected").size() == 1;
    }));
}

void OfflineGetPlayer() {
    t::Group("bridge-offline-getplayer");
    Rig r("offline");
    r.Start();
    r.Connect();
    r.Call("getPlayers", "{}");
    {
        std::lock_guard<std::mutex> g(r.fake.mu);
        r.fake.players.arr.pop_back();  // Guest leaves
    }
    CHECK(t::WaitFor([&] {
        JsonValue resp = r.Call("getPlayer", std::string("{\"gameId\":\"") + GUEST + "\"}");
        return resp.get("payload") && t::JsonTextEq(JsonDump(*resp.get("payload")),
                                                    std::string("{\"gameId\":\"") + GUEST + "\",\"name\":\"Guest\",\"steamId\":\"" +
                                                        GUEST + "\",\"platformId\":\"steam:" + GUEST + "\"}");
    }, 2000));
    JsonValue resp = r.Call("getPlayer", "{\"gameId\":\"123\"}");
    CHECK(resp.get("payload") && t::JsonTextEq(JsonDump(*resp.get("payload")), "{}"));
}

void CorruptOutbox() {
    t::Group("bridge-corrupt-outbox");
    Rig r("corrupt-outbox");
    std::string err;
    EnsureDirectory(r.StateDir(), err);
    t::WriteFile(r.StateDir() + "/event-outbox.json", "garbage");
    r.fake.PushEvent("chat-message", "{\"msg\":\"x\"}");
    r.Start();
    r.Connect();
    JsonValue resp = r.Call("getPlayers", "{}");
    CHECK(resp.get("payload"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    r.tr.Drain();
    CHECK(r.Events().empty());
    CHECK_EQ(File(r.StateDir() + "/event-outbox.json"), std::string("garbage"));
    CHECK(t::WaitFor([&] { return r.fake.Capability("connectorState").find("outbox") != std::string::npos; }));
    CHECK(t::WaitFor([&] { return r.Health().get("outbox") && !r.Health().get("outbox")->get("usable")->b; }));
}

}  // namespace

// ---- protocol 1 ----

// Opens a connection and returns the identify frame the bridge wrote.
JsonValue OpenForIdentify(Rig& r) {
    r.tr.Open();
    JsonValue identify;
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        auto f = r.tr.WireFrames("identify");
        if (f.empty()) return false;
        identify = f.back();
        return true;
    }));
    return identify;
}

uint64_t IdentifyV1(Rig& r) {
    uint64_t e = r.tr.Epoch();
    r.tr.Inject(R"({"type":"identifyResponse","payload":{"gameServerId":"gs-1","protocolVersion":1}})");
    CHECK(t::WaitFor([&] { return r.tr.identifiedEpoch == e; }));
    CHECK(t::WaitFor([&] { return r.HealthNum(nullptr, "protocol") == 1; }));
    return e;
}

void ProtocolV1Identify() {
    t::Group("bridge-v1-identify");
    Rig r("v1-identify");
    r.Start();
    JsonValue id = OpenForIdentify(r);
    const JsonValue& p = AsRecord(id.get("payload"));
    CHECK_EQ(JsonDump(*p.get("protocolVersions")), std::string("[0,1]"));
    const JsonValue* caps = p.get("capabilities");
    CHECK(caps && caps->type == JsonValue::Array && caps->arr.size() == 23);
    std::set<std::string> names;
    const JsonValue* map = nullptr;
    for (auto& c : caps->arr) {
        std::string n = c.get("name")->str;
        CHECK_MSG(std::regex_match(n, std::regex("^[a-z][a-zA-Z0-9]*(\\.[a-z][a-zA-Z0-9-]*)*$")), n);
        CHECK_MSG(names.insert(n).second, "duplicate " + n);
        if (n == "map") map = &c;
    }
    for (const char* need : {"players.list", "players.location", "items.give", "moderation.listBans", "server.shutdown",
                             "locations.list", "events.chat-message", "events.entity-killed"})
        CHECK_MSG(names.count(need), need);
    CHECK(!names.count("testReachability"));
    CHECK(map != nullptr);
    if (map) {
        CHECK(t::JsonTextEq(JsonDump(*map->get("parameters")),
                            R"({"dimensions":[{"id":"embervale","name":"Embervale",)"
                            R"("axes":{"horizontal":{"field":"x","direction":"right"},"vertical":{"field":"z","direction":"up"}},)"
                            R"("bounds":{"horizontal":{"min":0,"max":10240},"vertical":{"min":0,"max":10240}},)"
                            R"("source":{"type":"none"}}]})"));
    }
    CHECK(JsonDump(id).find("identity-secret-1") != std::string::npos);  // the identify itself carries the token
    IdentifyV1(r);
}

void ProtocolV1Responses() {
    t::Group("bridge-v1-responses");
    Rig r("v1-responses");
    r.Start();
    OpenForIdentify(r);
    IdentifyV1(r);
    std::string limon = std::string("{\"gameId\":\"") + LIMON + "\"}";
    // void actions answer payload null
    JsonValue resp = r.Call("kickPlayer", limon);
    CHECK_MSG(t::JsonTextEq(JsonDump(resp), R"({"type":"response","requestId":"req-1","payload":null})"), JsonDump(resp));
    resp = r.Call("sendMessage", R"({"message":"hello"})");
    CHECK(resp.get("payload") && resp.get("payload")->type == JsonValue::Null && !resp.get("error"));
    // positions carry the declared Dimension
    resp = r.Call("getPlayerLocation", limon);
    CHECK_MSG(t::JsonTextEq(JsonDump(*resp.get("payload")), R"({"x":10.5,"y":20,"z":-3,"dimension":"embervale"})"),
              JsonDump(resp));
    // an unknown player has no position: payload null, not an error
    resp = r.Call("getPlayerLocation", R"({"gameId":"76561198000009999"})");
    CHECK_MSG(resp.get("payload") && resp.get("payload")->type == JsonValue::Null && !resp.get("error"), JsonDump(resp));
    resp = r.Call("getPlayer", R"({"gameId":"76561198000009999"})");
    CHECK(resp.get("payload") && resp.get("payload")->type == JsonValue::Null);
    // failures use {"error":{"code","message"}} with a protocol 1 code
    auto code = [](const JsonValue& v) {
        const JsonValue& e = AsRecord(v.get("error"));
        return Str(e.get("code")).value_or("<none>");
    };
    resp = r.Call("sendMessage", "{}");
    CHECK_EQ(code(resp), std::string("invalid_args"));
    CHECK(Str(AsRecord(resp.get("error")).get("message")).value_or("") == "sendMessage requires 'message'");
    CHECK(!resp.get("payload"));
    resp = r.Call("getPlayerInventory", R"({"gameId":"76561198000009999"})");
    CHECK_EQ(code(resp), std::string("not_found"));
    resp = r.Call("fooBar", "{}");
    CHECK_EQ(code(resp), std::string("unsupported"));
    r.fake.unimplemented.insert("POST /teleport");
    resp = r.Call("teleportPlayer", std::string("{\"gameId\":\"") + LIMON + "\",\"x\":1,\"y\":2,\"z\":3}");
    CHECK_EQ(code(resp), std::string("unsupported"));
    std::string dup = r.Send("testReachability", "{}", "dup-1");
    r.Response(dup, resp);
    r.Send("testReachability", "{}", "dup-1");
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        auto f = r.tr.WireFrames("response", "dup-1");
        return f.size() == 2 && Str(AsRecord(f[1].get("error")).get("code")).value_or("") == "invalid_args";
    }));
    // non-void results are unchanged
    resp = r.Call("testReachability", "{}");
    CHECK(t::JsonTextEq(JsonDump(*resp.get("payload")), R"({"connectable":true,"reason":null})"));
}

std::string PayloadStream(const JsonValue& ev) { return Str(ev.get("payload")->get("streamId")).value_or(""); }
double PayloadSeq(const JsonValue& ev) { return Num(ev.get("payload")->get("seq")).value_or(-1); }

void ProtocolV1Events() {
    t::Group("bridge-v1-events");
    Rig r("v1-events");
    r.fake.PushEvent("chat-message", "{\"msg\":\"one\",\"channel\":\"global\"}");
    r.fake.PushEvent("chat-message", "{\"msg\":\"two\",\"channel\":\"global\"}");
    r.Start();
    OpenForIdentify(r);
    // nothing before identifyResponse
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    r.tr.Drain();
    CHECK(r.Events().empty());
    IdentifyV1(r);
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events().size() == 2;
    }));
    auto ev = r.Events();
    std::string stream = Str(r.Health().get("outbox")->get("streamId")).value_or("");
    CHECK(ValidStreamId(stream) && stream.size() == 36);
    CHECK(ev.size() == 2 && PayloadStream(ev[0]) == stream && PayloadStream(ev[1]) == stream);
    CHECK(ev.size() == 2 && PayloadSeq(ev[0]) == 1 && PayloadSeq(ev[1]) == 2);
    CHECK(ev.size() == 2 && t::JsonTextEq(JsonDump(*ev[0].get("payload")->get("data")), R"({"msg":"one","channel":"global"})"));
    // the ping watermark confirms nothing on protocol 1: only eventAck does
    r.tr.Ping();
    r.tr.Pong();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(r.HealthNum("outbox", "pending"), 2.0);
    r.tr.Inject(R"({"type":"eventAck","payload":{"streamId":"some-other-stream","upTo":2}})");
    r.tr.Inject("{\"type\":\"eventAck\",\"payload\":{\"streamId\":" + JsonStr(stream) + ",\"upTo\":1}}");
    CHECK(t::WaitFor([&] { return r.HealthNum("outbox", "pending") == 1; }));
    // a reconnect resends what is not acknowledged, with the same stream and seq
    r.tr.Close("tcp abort");
    r.tr.wire.clear();
    OpenForIdentify(r);
    IdentifyV1(r);
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events().size() == 1;
    }));
    ev = r.Events();
    CHECK(ev.size() == 1 && PayloadSeq(ev[0]) == 2 && PayloadStream(ev[0]) == stream &&
          ev[0].get("payload")->get("data")->get("msg")->str == "two");
    // new events continue the sequence
    r.fake.PushEvent("chat-message", "{\"msg\":\"three\",\"channel\":\"global\"}");
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events().size() == 2;
    }));
    ev = r.Events();
    CHECK(ev.size() == 2 && PayloadSeq(ev[1]) == 3);
    r.tr.Inject(R"({"type":"eventError","payload":{"streamId":"x","seq":3},"error":{"code":"invalid_args","message":"bad data"}})");
    r.tr.Inject("{\"type\":\"eventAck\",\"payload\":{\"streamId\":" + JsonStr(stream) + ",\"upTo\":3}}");
    CHECK(t::WaitFor([&] { return r.HealthNum("outbox", "pending") == 0 && r.HealthNum("outbox", "eventErrors") == 1; }));
    // the stream outlives a restart together with the outbox, so a resend after it still dedups
    r.fake.PushEvent("chat-message", "{\"msg\":\"four\",\"channel\":\"global\"}");
    CHECK(t::WaitFor([&] { return File(r.StateDir() + "/event-outbox.json").find("four") != std::string::npos; }));
    r.Stop();
    CHECK(File(r.StateDir() + "/event-outbox.json").find(stream) != std::string::npos);
    r.fake.NewBoot("boot-b");
    r.tr.wire.clear();
    r.Start();
    OpenForIdentify(r);
    IdentifyV1(r);
    CHECK(t::WaitFor([&] {
        r.tr.Drain();
        return r.Events().size() == 1;
    }));
    ev = r.Events();
    CHECK(ev.size() == 1 && PayloadStream(ev[0]) == stream && PayloadSeq(ev[0]) == 4);
}

void ProtocolV1Fallbacks() {
    t::Group("bridge-v1-fallbacks");
    {
        // Takaro without protocol 1 (no protocolVersion): protocol 0 shapes, ping-watermark confirmation
        Rig r("v1-old-takaro");
        r.fake.PushEvent("chat-message", "{\"msg\":\"old\"}");
        r.Start();
        r.Connect();
        CHECK_EQ(r.HealthNum(nullptr, "protocol"), 0.0);
        CHECK(t::WaitFor([&] {
            r.tr.Drain();
            return r.Events().size() == 1;
        }));
        CHECK(r.Events().size() == 1 && !r.Events()[0].get("payload")->get("seq"));
        JsonValue resp = r.Call("kickPlayer", std::string("{\"gameId\":\"") + LIMON + "\"}");
        CHECK(t::JsonTextEq(JsonDump(*resp.get("payload")), "{}"));
        resp = r.Call("sendMessage", "{}");
        CHECK(resp.get("error") && resp.get("error")->type == JsonValue::String);
        r.tr.Ping();
        r.tr.Pong();
        r.tr.Ping();
        r.tr.Pong();
        CHECK(t::WaitFor([&] { return r.HealthNum("outbox", "pending") == 0; }));
    }
    {
        // protocol 1 identify failure: top-level error, the code reaches health
        Rig r("v1-identify-error");
        r.Start();
        uint64_t e = r.tr.Open();
        r.tr.Inject(R"({"type":"identifyResponse","payload":null,"error":{"code":"invalid_args","message":"No registrationToken provided"}})");
        CHECK(t::WaitFor([&] { return !r.tr.CloseRequests().empty(); }));
        CHECK(!r.tr.CloseRequests().empty() && r.tr.CloseRequests()[0] == std::to_string(e) + ":identify rejected");
        bool seen = t::WaitFor([&] {
            JsonValue h = r.Health();
            const JsonValue* v = h.get("lastIdentifyError");
            return v && v->str.find("invalid_args: No registrationToken provided") != std::string::npos;
        });
        CHECK_MSG(seen, r.bridge->HealthJson().substr(0, 600));
    }
}

void RunBridgeTests() {
    IdentifyAndHeartbeat();
    IdentifyError();
    ReconfigureWhileRunning();
    EventsConfirmOnLaterPong();
    ReconnectResend();
    BacklogLatency();
    Adversarial();
    OverloadAndTimeouts();
    TimedBans();
    CorruptBanStore();
    BanIntentRecovery();
    ReconcileAndLocationFallback();
    LocationAfterLeave();
    WindowRules();
    LogTailFallback();
    LogFilter();
    OutboxSurvivesRestart();
    NewBootReconciles();
    PeriodicReconcileNeedsTwoMisses();
    OfflineGetPlayer();
    CorruptOutbox();
    ProtocolV1Identify();
    ProtocolV1Responses();
    ProtocolV1Events();
    ProtocolV1Fallbacks();
}
