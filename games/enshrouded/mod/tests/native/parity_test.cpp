// Mapping parity: replays tests/fixtures/parity-*.json (produced by the real sidecar, see
// sidecar/src/testing/parityFixtures.ts) against the native port and requires identical outputs.
#include "fake_plugin.h"
#include "fake_transport.h"
#include "native/adapter.h"
#include "native/bridge.h"
#include "native/logtail.h"
#include "native/mapping.h"
#include "testlib.h"

using namespace native;

namespace {

std::string g_fixtureDir;
int g_actionSteps = 0, g_bridgeSteps = 0, g_eventCases = 0, g_protocolCases = 0, g_logCases = 0;

JsonValue LoadFixture(const char* name) {
    std::string text = t::ReadFile(g_fixtureDir + "/parity-" + name + ".json");
    JsonValue v;
    if (text.empty() || !ParseJson(text, v)) {
        fprintf(stderr, "cannot load fixture parity-%s.json from %s\n", name, g_fixtureDir.c_str());
        exit(2);
    }
    return v;
}

void ApplySetup(t::FakePlugin& fake, const JsonValue* setup) {
    if (!setup) return;
    if (auto* h = setup->get("health")) fake.health = *h;
    if (auto* add = setup->get("addPlayers"))
        for (auto& p : add->arr) fake.players.arr.push_back(p);
    if (auto* rm = setup->get("removePlayers"))
        for (auto& id : rm->arr) {
            std::vector<JsonValue> kept;
            for (auto& p : fake.players.arr)
                if (!(p.get("gameId") && p.get("gameId")->str == id.str)) kept.push_back(p);
            fake.players.arr = kept;
        }
    if (auto* b = setup->get("bans")) fake.bans = *b;
    if (auto* u = setup->get("unimplemented"))
        for (auto& k : u->arr) fake.unimplemented.insert(k.str);
    if (auto* c = setup->get("commandResult")) fake.commandResult = *c;
}

std::string Describe(const std::string& caseName, size_t step, const JsonValue& s) {
    return caseName + " / step " + std::to_string(step) + " " + JsonDump(s).substr(0, 200);
}

void CheckRequest(t::FakePlugin& fake, const JsonValue& step, const std::string& where) {
    const JsonValue* req = step.get("request");
    if (!req) return;
    t::RecordedRequest r;
    bool found = fake.LastRequest(req->get("method")->str, req->get("path")->str, r);
    bool wantFound = req->get("found") && req->get("found")->b;
    CHECK_MSG(found == wantFound, where + " request found");
    if (!found || !wantFound) return;
    const JsonValue* body = req->get("body");
    if (body) CHECK_MSG(r.hasBody && t::JsonEq(r.body, *body), where + " body got " + (r.hasBody ? JsonDump(r.body) : "none"));
    else CHECK_MSG(!r.hasBody, where + " unexpected body");
}

void CheckReply(bool ok, const JsonValue& payload, const std::string& error, const JsonValue& step, const std::string& where) {
    const JsonValue* expect = step.get("expect");
    if (const JsonValue* e = expect->get("error")) {
        CHECK_MSG(!ok && error == e->str, where + " error got [" + (ok ? "ok " + JsonDump(payload) : error) + "] want [" + e->str + "]");
    } else {
        JsonValue want = expect->get("payload") ? *expect->get("payload") : JObj();
        JsonValue got = payload.type == JsonValue::Null ? JObj() : payload;
        CHECK_MSG(ok && t::JsonEq(got, want), where + " payload got " + (ok ? JsonDump(got) : "error " + error) + " want " + JsonDump(want));
    }
}

void ActionsDirect(const JsonValue& fx) {
    for (auto& c : fx.get("cases")->arr) {
        t::FakePlugin fake;
        ApplySetup(fake, c.get("setup"));
        ActionView view;
        size_t i = 0;
        for (auto& step : c.get("steps")->arr) {
            std::string where = Describe(c.get("name")->str, i++, step);
            JsonValue args = NormalizeArgs(step.get("args"));
            ActionOutcome out = ExecuteAction(fake, step.get("action")->str, args, view, 0);
            CheckReply(out.ok, out.payload, out.error, step, where);
            CheckRequest(fake, step, where);
            g_actionSteps++;
        }
    }
}

// The same fixtures through the whole bridge: request frame in, response frame out.
void ActionsThroughBridge(const JsonValue& fx) {
    int n = 0;
    for (auto& c : fx.get("cases")->arr) {
        t::FakePlugin fake;
        ApplySetup(fake, c.get("setup"));
        t::LoopbackTransport tr;
        std::string dir = t::TempDir("parity-bridge");
        fake.baseDir = dir;
        Store store(ResolveStatePaths(dir, [](const char*) { return std::string(); }));
        BridgeOptions o;
        o.config.identityToken = "id";
        o.config.registrationToken = "reg";
        o.config.logTail = LogTailMode::Never;
        o.config.actionTimeoutMs = 5000;
        o.game = &fake;
        o.transport = &tr;
        o.store = &store;
        Bridge bridge(o);
        bridge.Start();
        uint64_t epoch = tr.Open();
        (void)epoch;
        tr.Inject(R"({"type":"identifyResponse","payload":{"gameServerId":"gs"}})");
        size_t i = 0;
        for (auto& step : c.get("steps")->arr) {
            std::string where = "[bridge] " + Describe(c.get("name")->str, i++, step);
            std::string rid = "req-" + std::to_string(++n);
            JsonValue req = JObj();
            Put(req, "type", JStr("request"));
            Put(req, "requestId", JStr(rid));
            JsonValue payload = JObj();
            Put(payload, "action", *step.get("action"));
            Put(payload, "args", step.get("args") ? *step.get("args") : JNull());
            Put(req, "payload", payload);
            tr.Inject(JsonDump(req));
            JsonValue resp;
            bool got = t::WaitFor([&] {
                tr.Drain();
                auto frames = tr.WireFrames("response", rid);
                if (frames.empty()) return false;
                resp = frames.back();
                return true;
            });
            CHECK_MSG(got, where + " no response");
            if (!got) continue;
            const JsonValue* err = resp.get("error");
            CheckReply(!err, resp.get("payload") ? *resp.get("payload") : JNull(), err ? err->str : "", step, where);
            CheckRequest(fake, step, where);
            g_bridgeSteps++;
        }
        bridge.Stop();
    }
}

void Events(const JsonValue& fx) {
    for (auto& c : fx.get("cases")->arr) {
        const JsonValue* in = c.get("input");
        std::string where = "event " + JsonDump(*in).substr(0, 160);
        std::string type = in->get("type")->str;
        try {
            auto mapped = MapPluginEvent(type, in->get("data"));
            if (const JsonValue* e = c.get("error")) {
                CHECK_MSG(false, where + " expected error " + e->str);
            } else if (c.get("expect")->type == JsonValue::Null) {
                CHECK_MSG(!mapped, where + " expected null");
            } else {
                CHECK_MSG(mapped && mapped->type == c.get("expect")->get("type")->str, where + " type");
                if (mapped)
                    CHECK_MSG(t::JsonEq(mapped->data, *c.get("expect")->get("data")),
                              where + " got " + JsonDump(mapped->data) + " want " + JsonDump(*c.get("expect")->get("data")));
            }
        } catch (const std::exception& ex) {
            const JsonValue* e = c.get("error");
            CHECK_MSG(e && e->str == ex.what(), where + " threw [" + ex.what() + "]");
        }
        g_eventCases++;
    }
}

void Protocol(const JsonValue& fx) {
    for (auto& c : fx.get("normalizeArgs")->arr) {
        JsonValue got = NormalizeArgs(c.get("input"));
        CHECK_MSG(t::JsonEq(got, *c.get("expect")), "normalizeArgs " + JsonDump(*c.get("input")) + " got " + JsonDump(got));
        g_protocolCases++;
    }
    for (auto& c : fx.get("frames")->arr) {
        std::string kind = c.get("kind")->str;
        const JsonValue* in = c.get("input");
        std::string got;
        if (kind == "identify") got = CreateIdentify(in->get("identityToken")->str, in->get("registrationToken")->str, in->get("serverName")->str);
        else if (kind == "response") got = CreateResponse(in->get("requestId")->str, *in->get("payload"));
        else if (kind == "error") got = CreateErrorResponse(in->get("requestId")->str, in->get("error")->str);
        else got = CreateGameEvent(in->get("type")->str, *in->get("data"));
        CHECK_MSG(t::JsonTextEq(got, JsonDump(*c.get("expect"))), kind + " got " + got);
        g_protocolCases++;
    }
    for (auto& c : fx.get("entityTypes")->arr) {
        CHECK_MSG(MapEntityType(c.get("input")) == c.get("expect")->str, "entity type " + JsonDump(*c.get("input")));
        g_protocolCases++;
    }
}

void Logs(const JsonValue& fx) {
    for (auto& c : fx.get("forward")->arr) {
        LogEventsMode mode;
        ParseLogEventsMode(c.get("mode")->str, mode);
        JsonValue data = JObj();
        if (c.get("msg")) Put(data, "msg", *c.get("msg"));
        CHECK_MSG(ShouldForwardLog(mode, data) == c.get("expect")->b, "forward " + JsonDump(c));
        g_logCases++;
    }
    for (auto& c : fx.get("tail")->arr) {
        LogTailMode mode;
        ParseLogTailMode(c.get("mode")->str, mode);
        const JsonValue* h = c.get("health");
        bool got = ShouldTailLog(mode, h && h->type != JsonValue::Null ? h : nullptr);
        CHECK_MSG(got == c.get("expect")->b, "tail " + JsonDump(c));
        g_logCases++;
    }
    for (auto& c : fx.get("parser")->arr) {
        LogParser p;
        JsonValue got = JArr();
        for (auto& line : c.get("lines")->arr)
            for (auto& ev : p.Feed(line.str)) {
                JsonValue o = JObj();
                Put(o, "type", JStr(ev.type));
                Put(o, "data", ev.data);
                got.arr.push_back(o);
            }
        CHECK_MSG(t::JsonEq(got, *c.get("expect")), "log parser " + c.get("name")->str + " got " + JsonDump(got));
        g_logCases++;
    }
}

}  // namespace

void RunParityTests(const std::string& fixtureDir) {
    g_fixtureDir = fixtureDir;
    t::Group("parity-actions");
    JsonValue actions = LoadFixture("actions");
    ActionsDirect(actions);
    t::Group("parity-actions-bridge");
    ActionsThroughBridge(actions);
    t::Group("parity-events");
    Events(LoadFixture("events"));
    t::Group("parity-protocol");
    Protocol(LoadFixture("protocol"));
    t::Group("parity-logs");
    Logs(LoadFixture("logs"));
    printf("parity: %d action steps (direct), %d action steps (through bridge), %d event cases, %d protocol cases, %d log cases\n",
           g_actionSteps, g_bridgeSteps, g_eventCases, g_protocolCases, g_logCases);
}
