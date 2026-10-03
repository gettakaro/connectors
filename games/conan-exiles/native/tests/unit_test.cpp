// Host unit tests for everything that does not need a game process or a socket: JSON and the
// Takaro protocol shapes, config (env first, takaro.json, fail closed, redaction), the durable
// outbox, the frame queues and heartbeat, the pins scanner and its refusal rules, the Conan
// adapter (argument shapes, refusal, pending actions), the coverage registry, text and the
// ChatRpcData layout, and the ELF / /proc/self/maps helpers.
#include "common.h"
#include "conan/adapter.h"
#include "conan/coverage.h"
#include "conan/text.h"
#include "elfscan.h"
#include "pins/pins.h"
#include "takaro/config.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"
#include "takaro/outbox.h"
#include "takaro/protocol.h"
#include "takaro/transport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>

using namespace conan;

static int g_failed = 0, g_ran = 0;

#define CHECK(cond, ...)                                                \
    do {                                                                \
        g_ran++;                                                        \
        if (!(cond)) {                                                  \
            g_failed++;                                                 \
            printf("FAIL %s:%d: %s\n     ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)
#define EQ(a, b) CHECK((a) == (b), "got '%s' want '%s'", std::string(a).c_str(), std::string(b).c_str())

static JsonValue J(const std::string& text) {
    JsonValue v;
    if (!JsonParse(text, v)) printf("bad test json: %s\n", text.c_str());
    return v;
}

static void TestProtocol() {
    using namespace takaro;
    EQ(JsonDump(NormalizeArgs(nullptr)), "{}");
    JsonValue arr = J("[]"), nul = J("null"), str = J("\"{\\\"message\\\":\\\"hi\\\"}\""), blank = J("\"  \"");
    JsonValue obj = J("{\"a\":1}"), bad = J("\"{oops\""), strArr = J("\"[1,2]\"");
    EQ(JsonDump(NormalizeArgs(&arr)), "{}");
    EQ(JsonDump(NormalizeArgs(&nul)), "{}");
    EQ(JsonDump(NormalizeArgs(&str)), "{\"message\":\"hi\"}");
    EQ(JsonDump(NormalizeArgs(&blank)), "{}");
    EQ(JsonDump(NormalizeArgs(&obj)), "{\"a\":1}");
    EQ(JsonDump(NormalizeArgs(&bad)), "{}");
    EQ(JsonDump(NormalizeArgs(&strArr)), "{}");

    EQ(PlayerId(J("{\"gameId\":\"7656\"}")), "7656");
    EQ(PlayerId(J("{\"gameId\":null,\"player\":{\"gameId\":\"p1\"}}")), "p1");
    EQ(PlayerId(J("{\"playerRef\":{\"steamId\":\"s1\"}}")), "s1");
    EQ(PlayerId(J("{\"player\":null}")), "");
    EQ(StripSteamPrefix("steam:76561198000000001"), "76561198000000001");
    EQ(StripSteamPrefix("Platform:Steam:76561198000000001"), "76561198000000001");
    EQ(StripSteamPrefix("eos:abc"), "eos:abc");

    JsonValue v;
    CHECK(JsonParse(CreateIdentify("i", "r", "n"), v), "identify json");
    EQ(JsonDump(v), "{\"type\":\"identify\",\"payload\":{\"identityToken\":\"i\",\"registrationToken\":\"r\",\"name\":\"n\"}}");
    EQ(CreateResponse("r1", JsonValue()), "{\"type\":\"response\",\"requestId\":\"r1\",\"payload\":{}}");
    EQ(CreateErrorResponse("r2", "bad \"x\""), "{\"type\":\"response\",\"requestId\":\"r2\",\"error\":\"bad \\\"x\\\"\"}");
    EQ(CreateGameEvent("log", J("{\"msg\":\"m\"}")), "{\"type\":\"gameEvent\",\"payload\":{\"type\":\"log\",\"data\":{\"msg\":\"m\"}}}");
    CHECK(IsAction("getMapTile") && IsAction("shutdown") && !IsAction("rcon"), "actions");
    CHECK(IsEventType("entity-killed") && !IsEventType("player-joined"), "events");

    int64_t ms = 0;
    CHECK(ParseIsoMs("2026-10-03T10:00:00.250Z", ms) && ms == 1791021600250LL, "iso %lld", (long long)ms);
    CHECK(ParseIsoMs("2026-10-03T12:00:00+02:00", ms) && ms == 1791021600000LL, "offset");
    CHECK(!ParseIsoMs("2026-10-03T10:00:00", ms), "no zone refused");
    EQ(FormatIsoMs(1791021600250LL), "2026-10-03T10:00:00.250Z");
}

static void TestConfig() {
    using namespace takaro;
    std::map<std::string, std::string> envs;
    EnvFn env = [&](const char* n) { return envs.count(n) ? envs[n] : std::string(); };
    const std::string saved = "/srv/conan/ConanSandbox/Saved";
    EQ(ConfigFilePath(saved, env), "/srv/conan/ConanSandbox/Saved/Config/Takaro/takaro.json");

    Config c = LoadConfig(saved, env, "", false);
    CHECK(!c.enabled && c.disabledReason.find("identityToken and registrationToken") != std::string::npos, "%s",
          c.disabledReason.c_str());
    c = LoadConfig(saved, env,
                   "{\"identityToken\":\"file-id-1234\",\"registrationToken\":\"file-reg-5678\",\"serverName\":\"F\","
                   "\"caFile\":\"ca.pem\",\"_comment\":\"x\"}",
                   true);
    CHECK(c.enabled, "%s", c.disabledReason.c_str());
    EQ(c.identityToken, "file-id-1234");
    EQ(c.serverName, "F");
    EQ(c.caFile, saved + "/ca.pem");
    EQ(c.stateDir, saved + "/Takaro/state");
    EQ(c.url, "wss://connect.takaro.io/");
    envs["TAKARO_IDENTITY_TOKEN"] = "env-id-9999";
    envs["TAKARO_WS_URL"] = "wss://example.test/ws";
    c = LoadConfig(saved, env, "{\"identityToken\":\"file-id-1234\",\"registrationToken\":\"file-reg-5678\"}", true);
    CHECK(c.enabled, "env+file");
    EQ(c.identityToken, "env-id-9999");
    EQ(c.registrationToken, "file-reg-5678");
    EQ(c.url, "wss://example.test/ws");
    std::string summary = ConfigSummaryJson(c);
    CHECK(summary.find("env-id-9999") == std::string::npos && summary.find("file-reg-5678") == std::string::npos,
          "summary leaks a token: %s", summary.c_str());
    CHECK(summary.find("\"identityToken\":\"env TAKARO_IDENTITY_TOKEN\"") != std::string::npos, "%s", summary.c_str());
    EQ(c.Redact("error: token env-id-9999 and file-reg-5678 rejected"), "error: token [redacted] and [redacted] rejected");

    c = LoadConfig(saved, env, "{\"identityToken\": oops", true);
    CHECK(!c.enabled && c.disabledReason.find("failing closed") != std::string::npos, "malformed file fails closed");
    c = LoadConfig(saved, env, "[1]", true);
    CHECK(!c.enabled, "non-object file fails closed");
    c = LoadConfig(saved, env, "{\"registrationToken\":5}", true);
    CHECK(!c.enabled && c.disabledReason.find("must be a string") != std::string::npos, "%s", c.disabledReason.c_str());
    c = LoadConfig(saved, env, "{\"registrationToken\":\"r-1234\",\"bogus\":\"1\"}", true);
    CHECK(c.enabled && c.warnings.size() == 1, "unknown key warns");
    envs["TAKARO_WS_URL"] = "ws://plain/";
    c = LoadConfig(saved, env, "{\"registrationToken\":\"r-1234\"}", true);
    CHECK(!c.enabled && c.disabledReason.find("wss://") != std::string::npos, "plaintext refused");
    envs["TAKARO_WS_URL"] = "";
    envs["TAKARO_CONAN_NATIVE_DISABLE"] = "1";
    c = LoadConfig(saved, env, "{\"registrationToken\":\"r-1234\"}", true);
    CHECK(!c.enabled && c.disabledReason.find("DISABLE") != std::string::npos, "kill switch");
    envs.clear();
    envs["TAKARO_CONAN_CONFIG"] = "/etc/takaro/conan.json";
    EQ(ConfigFilePath(saved, env), "/etc/takaro/conan.json");
    envs["TAKARO_RECONNECT_BASE_MS"] = "abc";
    c = LoadConfig(saved, env, "", false);
    CHECK(c.reconnectBaseMs == 2000 && !c.warnings.empty(), "bad number warns and defaults");
}

static void TestOutbox() {
    using namespace takaro;
    char tmpl[] = "/tmp/conan-outbox-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    {
        Store s(dir + "/state");
        std::string notes = s.Load();
        CHECK(notes.find("created") != std::string::npos && s.OutboxUsable(), "%s", notes.c_str());
        for (int i = 1; i <= 3; i++)
            CHECK(s.Admit("chat-message", CreateGameEvent("chat-message", J("{\"msg\":\"m" + std::to_string(i) + "\"}"))) ==
                      (uint64_t)i,
                  "ids");
        CHECK(s.SaveOutbox(), "save");
        CHECK(s.ConfirmThrough(1) == 1 && s.Outbox().pending.size() == 2, "confirm 1");
        CHECK(s.ConfirmThrough(1) == 0, "confirm is idempotent");
        CHECK(s.SaveOutbox(), "save");
    }
    {
        Store s(dir + "/state");
        std::string notes = s.Load();
        CHECK(s.Outbox().pending.size() == 2 && s.Outbox().pending.front().id == 2 && s.Outbox().nextId == 4,
              "reload keeps the unconfirmed tail: %s", notes.c_str());
        CHECK(s.Outbox().confirmedTotal == 1, "confirmed total persisted");
    }
    {
        std::string err;
        AtomicWriteFile(dir + "/state/event-outbox.json", "{\"version\":1,\"nextId\":", err);
        Store s(dir + "/state");
        s.Load();
        CHECK(!s.OutboxUsable() && s.Errors().count("outbox"), "corrupt outbox is fenced");
        CHECK(!s.SaveOutbox(), "fenced outbox is never overwritten");
        std::string text;
        bool exists;
        ReadWholeFile(dir + "/state/event-outbox.json", text, exists, err);
        EQ(text, "{\"version\":1,\"nextId\":");
    }
    {
        Store s(dir + "/full");
        s.Load();
        for (size_t i = 0; i < Store::kMaxPendingEvents; i++) s.Admit(i == 10 ? "log" : "chat-message", "{}");
        s.Admit("player-death", "{}");
        CHECK(s.Outbox().losses == 1 && s.Outbox().pending.size() == Store::kMaxPendingEvents, "bounded");
        bool logLeft = false;
        for (auto& e : s.Outbox().pending) logLeft = logLeft || e.type == "log";
        CHECK(!logLeft, "the log event is dropped first");
    }
    std::string text = OutboxJson(OutboxState());
    OutboxState o;
    std::string err;
    CHECK(ParseOutbox(text, o, err), "%s", err.c_str());
    CHECK(!ParseOutbox("{\"version\":2,\"nextId\":1,\"pending\":[]}", o, err), "version");
    CHECK(!ParseOutbox("{\"version\":1,\"nextId\":3,\"pending\":[{\"id\":2,\"type\":\"log\",\"frame\":\"{}\"},"
                       "{\"id\":1,\"type\":\"log\",\"frame\":\"{}\"}]}",
                       o, err),
          "order");
}

static void TestQueuesAndHeartbeat() {
    using namespace takaro;
    FrameQueues q;
    auto f = [](FrameKind k, const std::string& t) {
        OutFrame o;
        o.kind = k;
        o.text = std::make_shared<const std::string>(t);
        return o;
    };
    q.Push(f(FrameKind::Event, "e"));
    q.Push(f(FrameKind::Response, "r"));
    q.Push(f(FrameKind::Control, "c"));
    q.Push(f(FrameKind::CriticalResponse, "x"));
    std::string order;
    OutFrame out;
    while (q.Pop(out)) order += *out.text;
    EQ(order, "cxre");
    CHECK(q.Push(f(FrameKind::Response, std::string(kMaxOutboundFrame + 1, 'a'))) == QueueStatus::TooLarge, "too large");
    for (int i = 0; i < 64; i++) q.Push(f(FrameKind::Control, "c"));
    CHECK(q.Push(f(FrameKind::Control, "c")) == QueueStatus::Full, "control bound");

    Heartbeat::Params p;
    Heartbeat h(p);
    h.Reset(0);
    CHECK(!h.PingDue(100, true), "nothing to confirm yet");
    h.OnEventWritten(3);
    CHECK(!h.PingDue(500, true) && h.PingDue(1000, true), "eager ping 1 s after a burst");
    CHECK(h.NextPingId() == 1, "first ping id");
    h.OnPingSent(1000);  // ping 1 covers events <= 3
    h.OnEventWritten(5);
    h.OnPingSent(2000);  // ping 2 covers <= 5
    h.OnEventWritten(9);
    h.OnPingSent(3000);  // ping 3 covers <= 9
    CHECK(h.OnPongId(7) == 0 && h.Outstanding() == 3, "unknown pong confirms nothing");
    CHECK(h.OnPongId(2) == 5 && h.Outstanding() == 1, "pong 2 confirms through 5 and retires ping 1");
    CHECK(h.OnPongId(1) == 0, "an already retired ping confirms nothing");
    CHECK(h.Stalled(3000 + p.intervalMs) == false, "one outstanding is not stalled");
    h.OnPingSent(4000);
    CHECK(h.Stalled(4000 + p.intervalMs) && !h.Stalled(4000 + p.intervalMs - 1), "two unanswered for an interval");
    CHECK(h.OnPongId(4) == 9 && h.Confirmed() == 9, "pong 4 confirms through 9");
    h.OnInbound(4000);
    CHECK(!h.Dead(4000 + p.idleMs) && h.Dead(4001 + p.idleMs), "idle watchdog");
}

static void TestPins() {
    std::vector<int> pat;
    CHECK(pins::ParsePattern("48 8b ?? 05", pat) && pat.size() == 4 && pat[2] == -1 && pat[1] == 0x8b, "parse");
    CHECK(!pins::ParsePattern("?? 48", pat), "leading wildcard refused");
    CHECK(!pins::ParsePattern("4", pat) && !pins::ParsePattern("zz", pat) && !pins::ParsePattern("", pat), "bad");
    CHECK(!pins::ParsePattern("488B", pat), "tokens need spaces");

    // A synthetic image holding the three linux signatures at known places.
    std::vector<uint8_t> img(0x4000, 0x90);
    const uintptr_t base = 0x400000;
    auto put = [&](size_t off, const std::string& pattern, std::map<size_t, uint32_t> fields) {
        std::vector<int> p;
        pins::ParsePattern(pattern, p);
        for (size_t i = 0; i < p.size(); i++) img[off + i] = p[i] < 0 ? 0xEE : (uint8_t)p[i];
        for (auto& kv : fields) memcpy(&img[off + kv.first], &kv.second, 4);
    };
    const auto& sigs = pins::SignaturesFor("linux");
    CHECK(sigs.size() == 3, "3 linux signatures");
    put(0x100, sigs[0].pattern, {{43, (uint32_t)(0x700000 - (base + 0x100 + 43 + 4))}});
    put(0x2000, sigs[2].pattern, {{9, 0x6a0000}});
    std::vector<pins::Region> regions = {{img.data(), img.size(), base}};
    pins::Result r = pins::Resolve("linux", "unknown-build", regions, false);
    CHECK(!r.ok && r.reason.find("unsupported server build (unknown-build)") != std::string::npos, "%s", r.reason.c_str());
    CHECK(r.anchors.processEvent == base + 0x100 && r.anchors.objObjects == 0x700000 && r.anchors.nameBlocks == 0x6a0000,
          "anchors %llx %llx %llx", (unsigned long long)r.anchors.processEvent,
          (unsigned long long)r.anchors.objObjects, (unsigned long long)r.anchors.nameBlocks);
    r = pins::Resolve("linux", "unknown-build", regions, true);
    CHECK(r.ok, "allowUnpinned accepts a clean scan of an unpinned build");
    r = pins::Resolve("linux", pins::PinnedBuilds()[0].buildId, regions, false);
    CHECK(!r.ok && r.reason.find("disagrees") != std::string::npos, "pinned build with other addresses refused: %s",
          r.reason.c_str());
    put(0x3000, sigs[2].pattern, {{9, 0x6a0000}});  // a second match
    r = pins::Resolve("linux", "unknown-build", regions, true);
    CHECK(!r.ok && r.reason.find("more than one match") != std::string::npos, "%s", r.reason.c_str());
    std::vector<pins::Region> none;
    r = pins::Resolve("linux", "x", none, true);
    CHECK(!r.ok && r.reason.find("signature scan failed (processEvent: no match)") != std::string::npos, "%s",
          r.reason.c_str());
    r = pins::Resolve("linux", "x", none, false);
    CHECK(!r.ok && r.reason.find("unsupported server build (x)") == 0 && r.reason.find("no match") != std::string::npos,
          "%s", r.reason.c_str());
    r = pins::Resolve("windows", "x", regions, true);
    CHECK(!r.ok && r.reason.find("no signatures for platform windows") != std::string::npos, "%s", r.reason.c_str());
    // A pattern straddling the end of a region must not read past it.
    std::vector<uint8_t> tail(10, 0x89);
    std::vector<pins::Region> small = {{tail.data(), tail.size(), 0x1000}};
    std::vector<int> p2;
    pins::ParsePattern(sigs[2].pattern, p2);
    CHECK(pins::FindMatches(small, p2, 2).empty(), "short region");
}

static void TestMaps() {
    std::string maps =
        "00400000-03ac8000 r--p 00000000 08:01 123 /srv/conan/ConanSandboxServer-Linux-Shipping\n"
        "03ac8000-0c106000 r-xp 036c7000 08:01 123 /srv/conan/ConanSandboxServer-Linux-Shipping\n"
        "0c10d000-0c228000 rw-p 0bf0b000 08:01 123 /srv/conan/ConanSandboxServer-Linux-Shipping\n"
        "7f0000000000-7f0000001000 r-xp 00000000 08:01 9 /usr/lib/libc.so.6\n"
        "7f0000002000-7f0000003000 r-xp 00000000 00:00 0 \n";
    auto r = linuxplat::ExecutableRegions("/srv/conan/ConanSandboxServer-Linux-Shipping", maps);
    CHECK(r.size() == 1 && r[0].address == 0x3ac8000 && r[0].size == 0x0c106000 - 0x3ac8000, "one text mapping");
    auto own = linuxplat::ExecutableRegions("/proc/self/exe");
    CHECK(own.empty(), "the path must be the real one, not /proc/self/exe");
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    own = linuxplat::ExecutableRegions(std::string(buf, n > 0 ? (size_t)n : 0));
    CHECK(!own.empty(), "this test binary has an executable mapping");
}

static void TestAdapter() {
    ChatRequest req;
    std::string err;
    CHECK(ParseSendMessage(J("{\"message\":\"hi\",\"opts\":{\"recipient\":{\"gameId\":\"76561198000000001\"},"
                             "\"senderNameOverride\":\" Bob \"}}"),
                           req, err),
          "%s", err.c_str());
    EQ(req.recipient, "76561198000000001");
    EQ(req.sender, "Bob");
    CHECK(ParseSendMessage(J("{\"message\":\"hi\",\"player\":{\"platformId\":\"steam:76561198000000002\"}}"), req, err),
          "nested");
    EQ(req.recipient, "76561198000000002");
    CHECK(ParseSendMessage(J("{\"message\":\"hi\",\"recipient\":\"werwerwer\"}"), req, err), "string recipient");
    EQ(req.recipient, "werwerwer");
    EQ(req.sender, "Takaro");
    CHECK(ParseSendMessage(J("{\"message\":\"hi\",\"opts\":null,\"gameId\":null}"), req, err) && req.recipient.empty(),
          "explicit nulls mean global");
    CHECK(!ParseSendMessage(J("{\"message\":\"   \"}"), req, err), "blank message");
    CHECK(!ParseSendMessage(J("{}"), req, err), "missing message");

    AdapterOptions ok;
    ok.ready = true;
    int sent = 0;
    ok.chat = [&](const ChatRequest&) {
        ChatOutcome o;
        o.success = true;
        sent++;
        return o;
    };
    Adapter a(ok);
    std::vector<takaro::GameEvent> evs;
    a.DrainEvents(evs);
    CHECK(evs.empty(), "a ready adapter sends no notice");
    auto r = a.Execute("testReachability", J("{}"));
    CHECK(r.ok && takaro::JsonDump(r.payload) == "{\"connectable\":true,\"reason\":null}", "reachable");
    r = a.Execute("sendMessage", J("{\"message\":\"x\"}"));
    CHECK(r.ok && sent == 1 && r.payload.type == JsonValue::Null, "sendMessage ok answers {}");
    r = a.Execute("giveItem", J("{}"));
    CHECK(!r.ok && r.error.find("giveItem is not implemented by the native Conan connector yet") == 0, "%s",
          r.error.c_str());
    r = a.Execute("getMapInfo", J("{}"));
    CHECK(r.ok && takaro::JsonDump(r.payload).find("\"enabled\":false") != std::string::npos, "map info");
    r = a.Execute("bogus", J("{}"));
    CHECK(!r.ok && r.error == "unknown action 'bogus'", "unknown");

    AdapterOptions no;
    no.refusal = "unsupported server build (abc)";
    Adapter b(no);
    b.DrainEvents(evs);
    CHECK(evs.size() == 1 && evs[0].type == "log" &&
              takaro::JsonDump(evs[0].data).find("CRITICAL: unsupported server build (abc)") != std::string::npos,
          "one critical notice");
    evs.clear();
    b.DrainEvents(evs);
    CHECK(evs.empty(), "only one");
    r = b.Execute("testReachability", J("{}"));
    CHECK(r.ok && takaro::JsonDump(r.payload).find("\"connectable\":false") != std::string::npos, "not reachable");
    for (int i = 0; i < takaro::kActionCount; i++) {
        std::string act = takaro::kActions[i];
        if (act == "testReachability") continue;
        r = b.Execute(act, J("{\"message\":\"x\"}"));
        CHECK(!r.ok && r.error.find("refused: unsupported server build") != std::string::npos, "%s: %s", act.c_str(),
              r.error.c_str());
    }
}

static void TestCoverage() {
    for (int i = 0; i < takaro::kActionCount; i++)
        CHECK(ActionCoverage(takaro::kActions[i]) != nullptr, "action %s covered", takaro::kActions[i]);
    for (int i = 0; i < takaro::kEventTypeCount; i++)
        CHECK(EventCoverage(takaro::kEventTypes[i]) != nullptr, "event %s covered", takaro::kEventTypes[i]);
    CHECK(std::string(ActionCoverage("sendMessage")->implementation) == "native", "chat native");
    CHECK(std::string(ActionCoverage("getMapInfo")->status) == "schema-fallback", "map fallback");
    CHECK(std::string(ActionCoverage("getMapTile")->status) == "unsupported", "tile unsupported");
    for (auto* a : {"giveItem", "kickPlayer", "banPlayer", "shutdown", "executeConsoleCommand"})
        CHECK(std::string(ActionCoverage(a)->implementation) == "pending", "%s still pending", a);
    for (auto* a : {"getPlayers", "getPlayer", "getPlayerLocation", "getPlayerInventory", "listItems", "listEntities",
                    "listLocations"})
        CHECK(std::string(ActionCoverage(a)->implementation) == "native", "%s native (lane L2a)", a);
    JsonValue v;
    CHECK(JsonParse(RegistryJson(), v), "registry json");
}

static void TestRecipient() {
    CHECK(IsSteam64("76561198000000001"), "steam64");
    CHECK(!IsSteam64("7656119800000000"), "short");
    CHECK(!IsSteam64("86561198000000001"), "prefix");
    CHECK(!IsSteam64("7656119800000000a"), "digits");
    EQ(NormalizeRecipient("steam:76561198000000001"), "76561198000000001");
    EQ(NormalizeRecipient(" 76561198000000001 "), "76561198000000001");
    EQ(NormalizeRecipient("some:name"), "some:name");
    EQ(NormalizeRecipient(""), "");
    CHECK(EqualsIgnoreCase("WerWer", "werwer") && !EqualsIgnoreCase("werwer", "werwe"), "ignore case");
}

static void TestText() {
    CHECK(ChatText("hello") == u"hello", "ascii");
    CHECK(ChatText("h\xc3\xa9llo") == u"h\u00e9llo", "2-byte");
    CHECK(ChatText("\xe2\x82\xac") == u"\u20ac", "3-byte");
    CHECK(ChatText("\xf0\x9f\x98\x80") == u"\U0001F600", "4-byte -> surrogate pair");
    CHECK(ChatText("a\xffz") == u"a\uFFFDz", "invalid byte");
    CHECK(ChatText("a\xc3") == u"a\uFFFD", "truncated sequence");
    CHECK(ChatText("\xc0\xaf") == u"\uFFFD\uFFFD", "overlong");
    CHECK(ChatText("\xed\xa0\x80") == u"\uFFFD\uFFFD\uFFFD", "encoded surrogate");
    CHECK(ChatText("a\nb\tc\rd") == u"a b c d", "whitespace");
    CHECK(ChatText(std::string("a\x01" "b\x7f" "c")) == u"abc", "control chars dropped");
    CHECK(ChatText(std::string(5000, 'x')).size() == kMaxMessageChars, "capped");
    CHECK(ChatText("abc\xf0\x9f\x98\x80", 4) == u"abc", "no half surrogate at the cap");
    EQ(Utf16To8(u"h\u00e9\u20ac\U0001F600"), "h\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
    EQ(Utf16To8(std::u16string(1, (char16_t)0xD800)), "\xef\xbf\xbd");
}

static void TestChatRpc() {
    CHECK(FileTimeTicks(0, 0) == 116444736000000000ULL, "unix epoch");
    CHECK(FileTimeTicks(1, 500) == 116444736000000000ULL + 10000000ULL + 5, "1s + 500ns");
    // The spike captured a real player message at 0x01dd529b80c4e16f on 2026-10-02 (UTC).
    uint64_t real = 0x01dd529b80c4e16fULL;
    int64_t sec = (int64_t)((real - FileTimeTicks(0, 0)) / 10000000ULL);
    CHECK(sec >= 1790899200 && sec < 1790985600, "captured timestamp is 2026-10-02: %lld", (long long)sec);

    std::u16string user = u"Takaro", channel = u"Global", msg = u"hello";
    alignas(16) uint8_t buf[ChatRpc::kSize];
    memset(buf, 0xAB, sizeof buf);
    PackChatRpc(buf, 0x1122334455667788ULL, user, channel, msg);
    uint64_t ts;
    memcpy(&ts, buf, 8);
    CHECK(ts == 0x1122334455667788ULL, "timestamp");
    for (size_t i = 8; i < ChatRpc::kUserName; i++) CHECK(buf[i] == 0, "ids zeroed at %zu", i);
    struct { size_t off; const std::u16string* s; } fields[] = {
        {ChatRpc::kUserName, &user}, {ChatRpc::kChannel, &channel}, {ChatRpc::kMessage, &msg}};
    for (auto& f : fields) {
        const char16_t* p;
        int32_t num, max;
        memcpy(&p, buf + f.off, 8);
        memcpy(&num, buf + f.off + 8, 4);
        memcpy(&max, buf + f.off + 12, 4);
        CHECK(p == f.s->c_str(), "FString data at %zx", f.off);
        CHECK(num == (int32_t)f.s->size() + 1 && max == num, "FString Num/Max include NUL at %zx", f.off);
        CHECK(p[num - 1] == 0, "NUL terminated");
    }
    CHECK(buf[ChatRpc::kGenerated] == 0, "generated false");
    for (size_t i = ChatRpc::kGenerated + 1; i < ChatRpc::kSize; i++) CHECK(buf[i] == 0, "tail zeroed");
}

static void TestBuildId() {
    std::string id = linuxplat::ReadElfBuildId("/proc/self/exe");
    CHECK(id.size() == 40 && id.find_first_not_of("0123456789abcdef") == std::string::npos, "own build-id '%s'",
          id.c_str());
    CHECK(linuxplat::ReadElfBuildId("/nonexistent").empty(), "missing file");
    CHECK(linuxplat::ReadElfBuildId("/proc/self/status").empty(), "not ELF");
}

int main() {
    TestProtocol();
    TestConfig();
    TestOutbox();
    TestQueuesAndHeartbeat();
    TestPins();
    TestMaps();
    TestAdapter();
    TestCoverage();
    TestRecipient();
    TestText();
    TestChatRpc();
    TestBuildId();
    printf("unit tests: %d/%d checks passed\n", g_ran - g_failed, g_ran);
    return g_failed ? 1 : 0;
}
