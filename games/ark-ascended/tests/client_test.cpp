// Fake Takaro and fake transport for takaro::Client: identify, args shapes, request correlation,
// deadlines, event acks, replay after reconnect, v0 fallback and token hygiene.
#include "../native/src/takaro_client.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace takaro;
using native::Notice;
using native::NoticeType;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

struct FakeTransport : native::ITransport {
    native::NoticeSink sink;
    std::vector<native::OutFrame> sent;
    native::QueueStatus next = native::QueueStatus::Accepted;
    uint64_t identifiedEpoch = 0;
    int closes = 0;
    bool Start(native::NoticeSink s) override { sink = std::move(s); return true; }
    void Stop() override {}
    native::QueueStatus Queue(native::OutFrame f) override {
        if (next == native::QueueStatus::Accepted) sent.push_back(std::move(f));
        return next;
    }
    void RequestClose(uint64_t, const std::string&) override { ++closes; }
    void MarkIdentified(uint64_t e) override { identifiedEpoch = e; }
    void Retarget(const std::string&, bool) override {}
    std::string StatsJson() override { return "{}"; }
};

struct Harness {
    FakeTransport t;
    std::vector<std::string> logs;
    std::unique_ptr<Client> c;
    explicit Harness(Config cfg = {}) {
        cfg.identityToken = "secret-identity";
        cfg.registrationToken = "secret-reg";
        cfg.wireDebug = true;
        c = std::make_unique<Client>(cfg, &t, "boot-1", [this](const std::string& l) { logs.push_back(l); });
        c->StartManual();
    }
    void Notify(NoticeType ty, uint64_t e, std::string text = {}, uint64_t outbox = 0) {
        Notice n;
        n.type = ty; n.epoch = e; n.text = std::move(text); n.outboxId = outbox;
        t.sink(std::move(n));
        c->PumpOnce();
    }
    void Open(uint64_t e, int protocol = 1) {
        Notify(NoticeType::Open, e);
        Notify(NoticeType::Frame, e, "{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs\",\"protocolVersion\":" + std::to_string(protocol) + "}}");
    }
    void Request(uint64_t e, const std::string& id, const std::string& action, const std::string& args) {
        Notify(NoticeType::Frame, e, "{\"type\":\"request\",\"requestId\":\"" + id + "\",\"payload\":{\"action\":\"" + action + "\",\"args\":" + args + "}}");
    }
    tj::Value Last() { return *tj::parse(*t.sent.back().text); }
    size_t CountEvents() {
        size_t n = 0;
        for (auto& f : t.sent) if (f.kind == native::FrameKind::Event) ++n;
        return n;
    }
};

static void test_identify() {
    Harness r;
    r.Notify(NoticeType::Open, 1);
    auto frame = r.Last();
    CHECK(frame.get("type")->as_string() == "identify");
    const auto* p = frame.get("payload");
    CHECK(p->get("identityToken")->as_string() == "secret-identity");
    CHECK(p->get("game")->as_string() == "ark-ascended");
    CHECK(p->get("protocolVersions")->is_array());
    CHECK(p->get("capabilities")->is_array());
    CHECK(!r.c->Identified());
    r.Notify(NoticeType::Frame, 1, "{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs\",\"protocolVersion\":1}}");
    CHECK(r.c->Identified());
    CHECK(r.c->ProtocolVersion() == 1);
    CHECK(r.c->GameServerId() == "gs");
    CHECK(r.t.identifiedEpoch == 1);
    for (const auto& l : r.logs) CHECK(l.find("secret") == std::string::npos);
}

static void test_identify_rejected() {
    Harness r;
    r.Notify(NoticeType::Open, 1);
    r.Notify(NoticeType::Frame, 1, "{\"type\":\"identifyResponse\",\"payload\":{\"error\":{\"message\":\"nope\"}}}");
    CHECK(!r.c->Identified());
    CHECK(r.t.closes == 1);
}

static void test_args_shapes() {
    const char* shapes[] = {"[]", "{}", "null", "\"{\\\"gameId\\\":\\\"x\\\"}\"", "\"not json\"", "7"};
    const char* ids[] = {"", "", "", "x", "", ""};
    for (size_t i = 0; i < 6; ++i) {
        Harness r;
        r.Open(1);
        r.Request(1, "r", "getPlayer", shapes[i]);
        GameCall call;
        CHECK(r.c->NextCall(call));
        CHECK(call.args.is_object());
        CHECK(PlayerIdOf(call.args) == ids[i]);
    }
    auto nested = tj::parse("{\"player\":{\"gameId\":\"p\",\"inventory\":[]},\"dimension\":null}");
    CHECK(PlayerIdOf(*nested) == "p");
    auto nullId = tj::parse("{\"gameId\":null}");
    CHECK(PlayerIdOf(*nullId).empty());
}

static void test_correlation_and_errors() {
    Harness r;
    r.Open(1);
    r.Request(1, "a", "getPlayers", "{}");
    r.Request(1, "b", "nope", "{}");
    GameCall a, b;
    CHECK(r.c->NextCall(a));
    CHECK(r.c->NextCall(b));
    r.c->Complete(b, Outcome::Fail("unsupported", "no"));
    r.c->PumpOnce();
    auto eb = r.Last();
    CHECK(eb.get("requestId")->as_string() == "b");
    CHECK(eb.has_key("payload") && eb.get("payload") == nullptr);  // get() reads explicit null as absent
    CHECK(eb.get("error")->get("code")->as_string() == "unsupported");
    r.c->Complete(a, Outcome::Ok(tj::Value::array()));
    r.c->PumpOnce();
    auto ea = r.Last();
    CHECK(ea.get("requestId")->as_string() == "a");
    CHECK(ea.get("payload")->is_array());
    CHECK(ea.get("error") == nullptr);
}

static void test_no_action_answered() {
    Harness r;
    r.Open(1);
    r.Notify(NoticeType::Frame, 1, "{\"type\":\"request\",\"requestId\":\"x\",\"payload\":{}}");
    CHECK(r.Last().get("error")->get("code")->as_string() == "invalid_args");
    GameCall c;
    CHECK(!r.c->NextCall(c));
}

static void test_reachability_off_game_thread() {
    Harness r;
    r.c->SetReachability([] { return tj::obj({{"connectable", tj::Value::boolean(true)}}); });
    r.Open(1);
    r.Request(1, "a", "testReachability", "{}");
    CHECK(r.Last().get("payload")->get("connectable")->as_bool());
    GameCall c;
    CHECK(!r.c->NextCall(c));
}

// A call reported as timed out must never run afterwards: that would give an item Takaro thinks failed.
static void test_deadline_blocks_late_run() {
    Config cfg;
    cfg.callDeadline = std::chrono::milliseconds(0);
    Harness r(cfg);
    r.Open(1);
    r.Request(1, "a", "giveItem", "{}");
    CHECK(r.Last().get("error")->get("code")->as_string() == "game_error");
    GameCall c;
    CHECK(!r.c->NextCall(c));
}

static void test_reconnect_drops_queued_calls() {
    Harness r;
    r.Open(1);
    r.Request(1, "a", "giveItem", "{}");
    r.Request(1, "b", "giveItem", "{}");
    GameCall taken;
    CHECK(r.c->NextCall(taken));  // already on the game thread when the connection drops
    r.Notify(NoticeType::Closed, 1, "");
    r.c->PumpOnce();
    GameCall c;
    CHECK(!r.c->NextCall(c));
    const size_t before = r.t.sent.size();
    r.Open(2);
    r.c->Complete(taken, Outcome::Ok(tj::Value::object()));
    r.c->PumpOnce();
    for (size_t i = before; i < r.t.sent.size(); ++i) CHECK(r.t.sent[i].text->find("\"requestId\":\"a\"") == std::string::npos);
}

static void test_late_complete_dropped() {
    Config cfg;
    cfg.callDeadline = std::chrono::milliseconds(30);
    Harness r(cfg);
    r.Open(1);
    r.Request(1, "a", "giveItem", "{}");
    GameCall c;
    CHECK(r.c->NextCall(c));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.c->PumpOnce();
    const size_t answered = r.t.sent.size();
    r.c->Complete(c, Outcome::Ok(tj::Value::object()));
    r.c->PumpOnce();
    CHECK(r.t.sent.size() == answered);  // one answer per request
}

static void test_events_ack_and_replay() {
    Harness r;
    r.Open(1);
    r.c->Emit("chat-message", tj::obj({{"msg", tj::str("one")}}));
    r.c->Emit("chat-message", tj::obj({{"msg", tj::str("two")}}));
    r.c->PumpOnce();
    CHECK(r.CountEvents() == 2);
    auto ev = r.Last();
    CHECK(ev.get("type")->as_string() == "gameEvent");
    const tj::Value* stamp = ev.get("payload")->get("data")->get("timestamp");
    CHECK(stamp != nullptr && stamp->as_string().size() == 24 && stamp->as_string().back() == 'Z');
    CHECK(ev.get("payload")->get("streamId")->as_string() == "boot-1");
    CHECK(ev.get("payload")->get("seq")->as_number() == 2);
    r.Notify(NoticeType::Frame, 1, "{\"type\":\"eventAck\",\"payload\":{\"streamId\":\"other\",\"upTo\":2}}");
    r.Notify(NoticeType::Frame, 1, "{\"type\":\"eventAck\",\"payload\":{\"streamId\":\"boot-1\",\"upTo\":1}}");
    r.Notify(NoticeType::Closed, 1, "gone");
    CHECK(!r.c->Identified());
    r.t.sent.clear();
    r.Open(2);
    CHECK(r.CountEvents() == 1);  // only the unacked event comes back
    for (auto& f : r.t.sent) {
        if (f.kind != native::FrameKind::Event) continue;
        CHECK(f.epoch == 2);
        auto e = tj::parse(*f.text);
        CHECK(e->get("payload")->get("seq")->as_number() == 2);
    }
}

static void test_events_wait_for_identify() {
    Harness r;
    r.c->Emit("log", tj::obj({{"msg", tj::str("early")}}));
    r.c->PumpOnce();
    CHECK(r.CountEvents() == 0);
    r.Open(1);
    CHECK(r.CountEvents() == 1);
}

static void test_event_window() {
    Config cfg;
    cfg.eventWindow = 2;
    Harness r(cfg);
    r.Open(1);
    for (int i = 0; i < 5; ++i) r.c->Emit("log", tj::obj({{"msg", tj::str("x")}}));
    r.c->PumpOnce();
    CHECK(r.CountEvents() == 2);
    r.Notify(NoticeType::Frame, 1, "{\"type\":\"eventAck\",\"payload\":{\"streamId\":\"boot-1\",\"upTo\":2}}");
    CHECK(r.CountEvents() == 4);
}

static void test_event_cap_counts_drops() {
    Config cfg;
    cfg.maxQueuedEvents = 3;
    Harness r(cfg);
    for (int i = 0; i < 5; ++i) r.c->Emit("log", tj::obj({{"msg", tj::str("x")}}));
    CHECK(r.c->DroppedEvents() == 2);
}

static void test_queue_refusal_retries() {
    Harness r;
    r.Open(1);
    r.t.next = native::QueueStatus::Full;
    r.c->Emit("log", tj::obj({{"msg", tj::str("x")}}));
    r.c->PumpOnce();
    CHECK(r.CountEvents() == 0);
    r.t.next = native::QueueStatus::Accepted;
    r.c->PumpOnce();
    CHECK(r.CountEvents() == 1);
}

static void test_v0() {
    Harness r;
    r.Open(1, 0);
    CHECK(r.c->ProtocolVersion() == 0);
    r.c->Emit("log", tj::obj({{"msg", tj::str("x")}}));
    r.c->PumpOnce();
    auto ev = r.Last();
    CHECK(ev.get("payload")->get("streamId") == nullptr);
    r.Request(1, "a", "x", "{}");
    GameCall c;
    CHECK(r.c->NextCall(c));
    const size_t before = r.t.sent.size();
    r.c->Complete(c, Outcome::Fail("game_error", "no"));
    r.c->PumpOnce();
    CHECK(r.t.sent.size() == before);  // v0 has no failure shape
    r.Notify(NoticeType::Confirmed, 1, {}, 1);
    r.Notify(NoticeType::Closed, 1);
    r.t.sent.clear();
    r.Open(2, 0);
    CHECK(r.CountEvents() == 0);  // pong-confirmed, so not replayed
}

static void test_garbage_ignored() {
    Harness r;
    r.Open(1);
    r.Notify(NoticeType::Frame, 1, "not json");
    r.Notify(NoticeType::Frame, 1, "[1,2]");
    r.Notify(NoticeType::Frame, 1, "{\"type\":\"unknown\"}");
    CHECK(r.c->Identified());
}

int main() {
    test_identify();
    test_identify_rejected();
    test_args_shapes();
    test_correlation_and_errors();
    test_no_action_answered();
    test_reachability_off_game_thread();
    test_deadline_blocks_late_run();
    test_reconnect_drops_queued_calls();
    test_late_complete_dropped();
    test_events_ack_and_replay();
    test_events_wait_for_identify();
    test_event_window();
    test_event_cap_counts_drops();
    test_queue_refusal_retries();
    test_v0();
    test_garbage_ignored();
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("client tests passed\n");
    return 0;
}
